import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parse } from '../src/opentype.mjs';
import { buildMathFont, buildFontWithDanglingMathOffset, GLYPH, NUM_GLYPHS } from './math-test-font.mjs';

// Every font enters through the public parse() entry point; no pre-parsed
// objects are handed to the new APIs.
function loadFont(options) {
    const bytes = buildMathFont(options);
    return parse(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength));
}

// Expand a GlyphAssembly part list exactly like the implementation must:
// extenders repeated r times, other parts once, declared order preserved.
function expandReference(parts, repeat) {
    const out = [];
    for (const part of parts) {
        if (part.flags & 1) {
            for (let i = 0; i < repeat; i += 1) {
                out.push(part);
            }
        } else {
            out.push(part);
        }
    }
    return out;
}

// Independent reference planner: enumerate small repetition counts and every
// legal integer overlap assignment per seam, then take the first repeat count
// that can reach the target. This deliberately does not share any code with
// src/mathstretch.mjs.
function referencePlan(baseParts, minOverlap, target) {
    for (let repeat = 0; repeat <= 12; repeat += 1) {
        const parts = expandReference(baseParts, repeat);
        if (parts.length === 0) {
            continue;
        }
        const seamCount = parts.length - 1;
        const caps = [];
        for (let i = 0; i < seamCount; i += 1) {
            caps.push(Math.min(parts[i].endConnectorLength, parts[i + 1].startConnectorLength));
        }
        if (caps.some(cap => cap < minOverlap)) {
            throw new Error('reference: illegal seam overlap');
        }
        const ranges = caps.map(cap => {
            const values = [];
            for (let v = minOverlap; v <= cap; v += 1) {
                values.push(v);
            }
            return values;
        });
        const fullAdvanceSum = parts.reduce((sum, part) => sum + part.fullAdvance, 0);
        let hit = null;
        (function enumerate(seam, overlaps) {
            if (hit) {
                return;
            }
            if (seam === seamCount) {
                const overlapSum = overlaps.reduce((a, b) => a + b, 0);
                const length = fullAdvanceSum - overlapSum;
                if (length >= target) {
                    hit = { repeat, parts, caps, overlaps, length, fullAdvanceSum };
                }
                return;
            }
            for (const value of ranges[seam]) {
                overlaps.push(value);
                enumerate(seam + 1, overlaps);
                overlaps.pop();
            }
        })(0, []);
        if (hit) {
            const minimumLength = fullAdvanceSum - caps.reduce((a, b) => a + b, 0);
            hit.minimumLength = minimumLength;
            return hit;
        }
    }
    return null;
}

// Recover the per-seam overlaps that a concrete plan implies through its
// offsets and the known fullAdvance of each placed part.
function overlapsFromPlan(font, plan) {
    const baseByGlyph = new Map();
    const collect = construction => {
        if (!construction.glyphAssembly) {
            return;
        }
        for (const part of construction.glyphAssembly.parts) {
            baseByGlyph.set(part.glyphIndex, part);
        }
    };
    for (const subtable of [font.tables.math.mathVariants.vertical, font.tables.math.mathVariants.horizontal]) {
        for (const construction of subtable.constructions) {
            collect(construction);
        }
    }
    const overlaps = [];
    for (let i = 1; i < plan.parts.length; i += 1) {
        const previous = plan.parts[i - 1];
        const current = plan.parts[i];
        const previousAdvance = baseByGlyph.get(previous.glyphIndex).fullAdvance;
        overlaps.push(previousAdvance - (current.offset - previous.offset));
    }
    return overlaps;
}

// A plan matches the brute-force reference when: it uses the same repeat
// count, the same part order, the advance follows the "shortest length wins
// if it already exceeds the target" rule, and every seam obeys both the
// connector bounds and the common water-level reduction rule.
function assertPlanMatchesReference(font, construction, minOverlap, target, plan) {
    const reference = referencePlan(construction.glyphAssembly.parts, minOverlap, target);
    assert.ok(reference, 'reference expected a feasible construction');
    assert.equal(plan.kind, 'assembly');

    const expanded = expandReference(construction.glyphAssembly.parts, reference.repeat);
    assert.equal(plan.parts.length, expanded.length, 'part count matches repeat count');
    for (let i = 0; i < expanded.length; i += 1) {
        assert.equal(plan.parts[i].glyphIndex, expanded[i].glyphIndex, 'part order is preserved');
    }
    for (const part of construction.glyphAssembly.parts) {
        if (part.flags & 1) {
            const occurrences = plan.parts.filter(p => p.glyphIndex === part.glyphIndex).length;
            assert.equal(occurrences, reference.repeat, 'every extender repeats the same r times');
        }
    }

    const expectedAdvance = Math.max(target, reference.minimumLength);
    assert.equal(plan.advance, expectedAdvance, 'advance is exact target or the shortest feasible length');
    assert.equal(plan.advance, plan.parts[plan.parts.length - 1].offset +
        baseAdvanceOf(construction, plan.parts[plan.parts.length - 1].glyphIndex),
        'advance is last offset plus last fullAdvance');
    assert.equal(plan.parts[0].offset, 0, 'offsets start at zero');

    const overlaps = overlapsFromPlan(font, plan);
    assert.equal(overlaps.length, reference.caps.length);
    const overlapSum = overlaps.reduce((a, b) => a + b, 0);
    assert.equal(overlapSum, reference.fullAdvanceSum - plan.advance, 'overlaps account for the advance');
    for (let i = 0; i < overlaps.length; i += 1) {
        assert.ok(overlaps[i] >= minOverlap, 'overlap never below minConnectorOverlap');
        assert.ok(overlaps[i] <= reference.caps[i], 'overlap never above the smaller connector');
    }
    // Water-level shape: one integer lambda must explain every seam as
    // max(minOverlap, cap - lambda). A single-part plan has no seams, so the
    // water-level condition is vacuously satisfied.
    if (reference.caps.length === 0) {
        assert.deepEqual(overlaps, []);
        return;
    }
    const maxCap = Math.max.apply(null, reference.caps);
    let waterLevelOk = false;
    for (let lambda = 0; lambda <= maxCap - minOverlap; lambda += 1) {
        if (overlaps.every((overlap, i) => overlap === Math.max(minOverlap, reference.caps[i] - lambda))) {
            waterLevelOk = true;
            break;
        }
    }
    assert.ok(waterLevelOk, 'overlap reduction follows one common water level');
}

function baseAdvanceOf(construction, glyphIndex) {
    const part = construction.glyphAssembly.parts.find(p => p.glyphIndex === glyphIndex);
    return part.fullAdvance;
}

function constructionFor(font, glyphIndex, direction) {
    const subtable = direction === 'vertical' ?
        font.tables.math.mathVariants.vertical : font.tables.math.mathVariants.horizontal;
    const coverage = subtable.coverage;
    let coverageIndex = -1;
    if (coverage.format === 1) {
        coverageIndex = coverage.glyphs.indexOf(glyphIndex);
    } else {
        for (const range of coverage.ranges) {
            if (glyphIndex >= range.start && glyphIndex <= range.end) {
                coverageIndex = range.index + (glyphIndex - range.start);
            }
        }
    }
    return subtable.constructions[coverageIndex];
}

test('math parse MATH table is read through the public SFNT parser', () => {
    const font = loadFont();
    assert.equal(font.numGlyphs, NUM_GLYPHS);
    const variants = font.tables.math.mathVariants;
    assert.equal(variants.minConnectorOverlap, 20);

    // Both coverage formats are exercised.
    assert.deepEqual(variants.vertical.coverage, {
        format: 2,
        ranges: [{ start: GLYPH.VBRACKET, end: GLYPH.VBRACKET, index: 0 }]
    });
    assert.deepEqual(variants.horizontal.coverage, {
        format: 1,
        glyphs: [GLYPH.HARROW, GLYPH.FROZEN, GLYPH.NEG_IC]
    });

    const arrow = constructionFor(font, GLYPH.HARROW, 'horizontal');
    assert.equal(arrow.mathGlyphVariants.length, 4);
    assert.equal(arrow.glyphAssembly.parts.length, 4);
    assert.equal(arrow.glyphAssembly.italicsCorrection, 35);
    const bracket = constructionFor(font, GLYPH.VBRACKET, 'vertical');
    assert.equal(bracket.glyphAssembly.parts.length, 3);

    // Fonts without a MATH table report null instead of throwing.
    const plainFont = loadFont({ includeMath: false });
    assert.equal(plainFont.tables.math, undefined);
    assert.equal(plainFont.getStretchGlyph(GLYPH.HARROW, 'horizontal', 100), null);
    assert.equal(plainFont.getStretchPath(GLYPH.HARROW, 'horizontal', 100), null);

    // Truncated table bytes must be rejected rather than read as another table.
    assert.throws(() => loadFont({ truncatedMath: true }), /MATH/);
    // An internal offset pointing past the table end must be rejected too.
    assert.throws(() => {
        const bytes = buildFontWithDanglingMathOffset();
        parse(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength));
    }, /MATH/);
    // An out-of-range glyph index inside a part record fails loudly on use.
    const badIndexFont = loadFont({ badPartGlyphIndex: true });
    assert.throws(() => badIndexFont.getStretchGlyph(GLYPH.FROZEN, 'horizontal', 100), /MATH/);
});

test('math variant unsorted records yield the smallest sufficient variant', () => {
    const font = loadFont();

    // Target 120: the 120/140/140/150 records qualify, so take the exact 120.
    let plan = font.getStretchGlyph(GLYPH.HARROW, 'horizontal', 120);
    assert.equal(plan.kind, 'variant');
    assert.equal(plan.advance, 120);
    assert.equal(plan.italicsCorrection, 0, 'single-glyph variants never report italics correction');
    assert.deepEqual(plan.parts, [{ glyphIndex: GLYPH.HARROW, offset: 0 }]);

    // Target 130: three records qualify at 140/140/150; the advance tie at
    // 140 must break towards the smaller glyph index (H_LEFT=3 < H_RIGHT=6),
    // proving the records are not assumed to be sorted.
    plan = font.getStretchGlyph(GLYPH.HARROW, 'horizontal', 130);
    assert.equal(plan.kind, 'variant');
    assert.equal(plan.advance, 140);
    assert.equal(plan.parts[0].glyphIndex, GLYPH.H_LEFT);

    // Target above every variant (largest declared advance is 170) falls
    // through to the assembly.
    plan = font.getStretchGlyph(GLYPH.HARROW, 'horizontal', 171);
    assert.equal(plan.kind, 'assembly');
    assert.equal(plan.italicsCorrection, 35);

    // Vertical construction uses the same rules with the other coverage.
    plan = font.getStretchGlyph(GLYPH.VBRACKET, 'vertical', 120);
    assert.equal(plan.kind, 'variant');
    assert.equal(plan.advance, 160);
    assert.equal(plan.parts[0].glyphIndex, GLYPH.VBRACKET_BIG);
    plan = font.getStretchGlyph(GLYPH.VBRACKET, 'vertical', 161);
    assert.equal(plan.kind, 'assembly');

    // Glyphs not covered in the requested direction return null.
    assert.equal(font.getStretchGlyph(GLYPH.PLAIN, 'horizontal', 100), null);
    assert.equal(font.getStretchGlyph(GLYPH.HARROW, 'vertical', 100), null);
    assert.equal(font.getStretchGlyph(GLYPH.VBRACKET, 'horizontal', 100), null);

    // Nothing reaches the target: RangeError, never the biggest variant.
    assert.throws(
        () => font.getStretchGlyph(GLYPH.FROZEN, 'horizontal', 5000),
        RangeError
    );

    // Argument validation.
    const badArguments = [
        [-1, 'horizontal', 100],
        [NUM_GLYPHS, 'horizontal', 100],
        [1.5, 'horizontal', 100],
        [GLYPH.HARROW, 'sideways', 100],
        [GLYPH.HARROW, 'horizontal', 0],
        [GLYPH.HARROW, 'horizontal', -10],
        [GLYPH.HARROW, 'horizontal', NaN],
        [GLYPH.HARROW, 'horizontal', Infinity],
        [GLYPH.HARROW, 'horizontal', '100']
    ];
    for (const args of badArguments) {
        assert.throws(() => font.getStretchGlyph(...args), RangeError);
        assert.throws(() => font.getStretchPath(...args), RangeError);
    }
});

test('math assembly plans match an exhaustive brute-force reference', () => {
    const font = loadFont();
    const minOverlap = font.tables.math.mathVariants.minConnectorOverlap;

    const arrow = constructionFor(font, GLYPH.HARROW, 'horizontal');
    // Arrow base parts: two non-extender end caps plus two distinct extender
    // geometries. At r = 0 the extenders vanish, leaving the two end caps and
    // exactly one connector seam between non-extenders.
    assertPlanMatchesReference(font, arrow, minOverlap, 180,
        font.getStretchGlyph(GLYPH.HARROW, 'horizontal', 180));
    assert.equal(font.getStretchGlyph(GLYPH.HARROW, 'horizontal', 180).parts.length, 2);
    // r = 1: four parts, both extender geometries appearing once each.
    assertPlanMatchesReference(font, arrow, minOverlap, 250,
        font.getStretchGlyph(GLYPH.HARROW, 'horizontal', 250));
    assert.equal(font.getStretchGlyph(GLYPH.HARROW, 'horizontal', 250).parts.length, 4);
    // r = 2: six parts and five seams with different connector caps.
    assertPlanMatchesReference(font, arrow, minOverlap, 400,
        font.getStretchGlyph(GLYPH.HARROW, 'horizontal', 400));
    assert.equal(font.getStretchGlyph(GLYPH.HARROW, 'horizontal', 400).parts.length, 6);

    const bracket = constructionFor(font, GLYPH.VBRACKET, 'vertical');
    // Vertical r = 0 keeps the bottom/top end caps only (one seam).
    assertPlanMatchesReference(font, bracket, minOverlap, 180,
        font.getStretchGlyph(GLYPH.VBRACKET, 'vertical', 180));
    assert.equal(font.getStretchGlyph(GLYPH.VBRACKET, 'vertical', 180).parts.length, 2);
    // r = 1 inserts the single extender once; r = 2 inserts it twice.
    assertPlanMatchesReference(font, bracket, minOverlap, 200,
        font.getStretchGlyph(GLYPH.VBRACKET, 'vertical', 200));
    assertPlanMatchesReference(font, bracket, minOverlap, 300,
        font.getStretchGlyph(GLYPH.VBRACKET, 'vertical', 300));

    // Extender-only assembly: r = 0 is an empty sequence and must be skipped.
    // A target within one part's fullAdvance lands on r = 1, which has no
    // seam at all; the advance is then simply that part's fullAdvance.
    const negative = constructionFor(font, GLYPH.NEG_IC, 'horizontal');
    const singlePartPlan = font.getStretchGlyph(GLYPH.NEG_IC, 'horizontal', 70);
    assert.equal(singlePartPlan.kind, 'assembly');
    assert.equal(singlePartPlan.parts.length, 1);
    assert.equal(singlePartPlan.advance, 80);
    assertPlanMatchesReference(font, negative, minOverlap, 70, singlePartPlan);
    const negPlan = font.getStretchGlyph(GLYPH.NEG_IC, 'horizontal', 90);
    assert.equal(negPlan.italicsCorrection, -40, 'negative italics correction is preserved');
    assertPlanMatchesReference(font, negative, minOverlap, 90, negPlan);
    assertPlanMatchesReference(font, negative, minOverlap, 200,
        font.getStretchGlyph(GLYPH.NEG_IC, 'horizontal', 200));

    // A construction without extenders has one fixed reachable interval and
    // must fail deterministically beyond it instead of looping forever.
    const frozen = constructionFor(font, GLYPH.FROZEN, 'horizontal');
    const frozenReference = referencePlan(frozen.glyphAssembly.parts, minOverlap, 176);
    assert.equal(frozenReference.repeat, 0);
    assertPlanMatchesReference(font, frozen, minOverlap, 176,
        font.getStretchGlyph(GLYPH.FROZEN, 'horizontal', 176));
    assert.throws(
        () => font.getStretchGlyph(GLYPH.FROZEN, 'horizontal', 181),
        RangeError
    );

    // Repeated queries are stable, and mutating a returned plan cannot leak
    // into the parsed table or later results.
    const snapshot = JSON.stringify(font.tables.math);
    const first = font.getStretchGlyph(GLYPH.HARROW, 'horizontal', 360);
    first.parts.length = 0;
    first.parts.push({ glyphIndex: 0xFFFF, offset: -1 });
    first.advance = -999;
    const second = font.getStretchGlyph(GLYPH.HARROW, 'horizontal', 360);
    const third = font.getStretchGlyph(GLYPH.VBRACKET, 'vertical', 300);
    assertPlanMatchesReference(font, arrow, minOverlap, 360, second);
    assertPlanMatchesReference(font, bracket, minOverlap, 300, third);
    assert.equal(JSON.stringify(font.tables.math), snapshot);
});

test('math path assembled Path control points follow getPath Y-flip rules', () => {
    const font = loadFont();
    // Same operation order Glyph.getPath uses internally, so bit-for-bit
    // identical control points can be compared.
    const scale = 1 / font.unitsPerEm * 72;

    // Horizontal plan: offsets grow to the right, no Y translation per part.
    const hPlan = font.getStretchGlyph(GLYPH.HARROW, 'horizontal', 360);
    const hPath = font.getStretchPath(GLYPH.HARROW, 'horizontal', 360, 11, 23, 72);
    const expectedHorizontal = [];
    for (const part of hPlan.parts) {
        const glyph = font.glyphs.get(part.glyphIndex);
        for (const cmd of glyph.path.commands) {
            // Glyph.getPath only emits Z for stroked paths; these glyphs are fills.
            if (cmd.type === 'Z') {
                continue;
            }
            expectedHorizontal.push(transformCommand(cmd, 11 + part.offset * scale, 23, scale));
        }
    }
    assert.equal(hPath.commands.length, expectedHorizontal.length);
    assert.deepEqual(hPath.commands, expectedHorizontal);

    // Vertical plan: font offsets grow upward, so screen origins move down
    // via the same Y flip Glyph.getPath uses.
    const vPlan = font.getStretchGlyph(GLYPH.VBRACKET, 'vertical', 300);
    const vPath = font.getStretchPath(GLYPH.VBRACKET, 'vertical', 300, 4, 90, 72);
    const expectedVertical = [];
    for (const part of vPlan.parts) {
        const glyph = font.glyphs.get(part.glyphIndex);
        for (const cmd of glyph.path.commands) {
            if (cmd.type === 'Z') {
                continue;
            }
            expectedVertical.push(transformCommand(cmd, 4, 90 - part.offset * scale, scale));
        }
    }
    assert.equal(vPath.commands.length, expectedVertical.length);
    assert.deepEqual(vPath.commands, expectedVertical);

    // Explicit check of the flip on a known font-space point (0, 60) of the
    // first vertical part (V_BOTTOM, a 40x60 rectangle): its top must render
    // above its baseline origin by exactly the flipped height.
    const firstTop = vPath.commands.find(cmd => cmd.type === 'L' && cmd.y < 90 && cmd.x === 4);
    assert.ok(firstTop);
    assert.equal(firstTop.y, 90 - 60 * scale);

    // Default arguments equal explicit x=0, y=0, fontSize=72.
    const defaultPath = font.getStretchPath(GLYPH.HARROW, 'horizontal', 176);
    const explicitPath = font.getStretchPath(GLYPH.HARROW, 'horizontal', 176, 0, 0, 72);
    assert.deepEqual(defaultPath.commands, explicitPath.commands);

    // The italics correction never moves parts: the negative-IC plan renders
    // at exactly the offsets the plan declares.
    const negPlan = font.getStretchGlyph(GLYPH.NEG_IC, 'horizontal', 139);
    assert.equal(negPlan.italicsCorrection, -40);
    const negPath = font.getStretchPath(GLYPH.NEG_IC, 'horizontal', 139, 0, 0, 72);
    const expectedNeg = [];
    for (const part of negPlan.parts) {
        const glyph = font.glyphs.get(part.glyphIndex);
        for (const cmd of glyph.path.commands) {
            if (cmd.type === 'Z') {
                continue;
            }
            expectedNeg.push(transformCommand(cmd, part.offset * scale, 0, scale));
        }
    }
    assert.deepEqual(negPath.commands, expectedNeg);

    // Unsupported glyphs give null; unreachable targets still throw.
    assert.equal(font.getStretchPath(GLYPH.PLAIN, 'horizontal', 100), null);
    assert.throws(() => font.getStretchPath(GLYPH.FROZEN, 'horizontal', 5000), RangeError);

    // Component glyphs and parsed tables are untouched by rendering.
    const partSnapshot = font.glyphs.get(GLYPH.H_EXT_1).path.commands.map(cmd => ({ ...cmd }));
    font.getStretchPath(GLYPH.HARROW, 'horizontal', 1200, 3, 5, 72);
    font.getStretchPath(GLYPH.VBRACKET, 'vertical', 900, 3, 5, 72);
    assert.deepEqual(font.glyphs.get(GLYPH.H_EXT_1).path.commands, partSnapshot);
});

// Mirror Glyph.getPath's coordinate transform for one path command.
// Both growth directions keep the standard screen Y flip; vertical
// placement is already encoded in originY by the caller.
function transformCommand(cmd, originX, originY, scale) {
    const mapPoint = (x, y) => ({
        x: originX + x * scale,
        // Both directions keep the standard getPath screen Y flip;
        // vertical placement is already encoded in originY.
        y: originY + (-y * scale)
    });
    if (cmd.type === 'M' || cmd.type === 'L') {
        return { type: cmd.type, ...mapPoint(cmd.x, cmd.y) };
    }
    if (cmd.type === 'Q') {
        return { type: cmd.type, ...mapPoint(cmd.x1, cmd.y1), ...mapPoint(cmd.x, cmd.y) };
    }
    if (cmd.type === 'C') {
        return {
            type: cmd.type,
            ...mapPoint(cmd.x1, cmd.y1),
            ...mapPoint(cmd.x2, cmd.y2),
            ...mapPoint(cmd.x, cmd.y)
        };
    }
    return { type: cmd.type };
}
