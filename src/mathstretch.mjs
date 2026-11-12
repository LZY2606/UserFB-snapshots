// Stretch-glyph layout based on the MATH table's MathVariants subtree.
// The output is expressed in font design units and is turned into real
// outlines by the regular Glyph.getPath() machinery; glyph outlines are
// never scaled non-uniformly.

import check from './check.mjs';
import mathTable from './tables/math.mjs';
import Path from './path.mjs';

const HORIZONTAL = 'horizontal';
const VERTICAL = 'vertical';

// Hard upper bound for the extender repetition count, so a construction
// that cannot keep growing fails deterministically instead of looping.
const MAX_REPEAT = 0xFFFF;

function validateArguments(glyphIndex, direction, targetAdvance, numGlyphs) {
    if (typeof glyphIndex !== 'number' || !Number.isInteger(glyphIndex) ||
        glyphIndex < 0 || glyphIndex >= numGlyphs) {
        throw new RangeError('glyphIndex must be an integer glyph index in this font.');
    }
    if (direction !== HORIZONTAL && direction !== VERTICAL) {
        throw new RangeError('direction must be "horizontal" or "vertical".');
    }
    if (typeof targetAdvance !== 'number' || !Number.isFinite(targetAdvance) || targetAdvance <= 0) {
        throw new RangeError('targetAdvance must be a positive finite number of font units.');
    }
}

// Expand a GlyphAssembly part list: every extender part is repeated r
// times, every other part once, keeping the declared order.
function expandParts(assemblyParts, repeat) {
    const parts = [];
    for (let i = 0; i < assemblyParts.length; i += 1) {
        const part = assemblyParts[i];
        if (part.flags & 1) {
            for (let j = 0; j < repeat; j += 1) {
                parts.push(part);
            }
        } else {
            parts.push(part);
        }
    }
    return parts;
}

// Distribute a required overlap reduction over the seams using a common
// water level: overlap_k = max(minConnectorOverlap, cap_k - lambda),
// so short connectors reach the floor before long ones give way further.
// Returns the per-seam overlaps with sum exactly equal to targetOverlap.
function waterFillOverlaps(caps, minOverlap, targetOverlap) {
    const seamCount = caps.length;
    const integral = Number.isInteger(targetOverlap) &&
        caps.every(v => Number.isInteger(v)) && Number.isInteger(minOverlap);
    let lo = 0;
    let hi = Math.max.apply(null, caps) - minOverlap;
    const overlapAt = lambda => {
        let sum = 0;
        for (let i = 0; i < seamCount; i += 1) {
            sum += Math.max(minOverlap, caps[i] - lambda);
        }
        return sum;
    };
    for (let i = 0; i < 64; i += 1) {
        const mid = (lo + hi) / 2;
        if (overlapAt(mid) >= targetOverlap) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    const lambda = (lo + hi) / 2;
    const overlaps = new Array(seamCount);
    for (let i = 0; i < seamCount; i += 1) {
        overlaps[i] = Math.max(minOverlap, caps[i] - lambda);
    }
    if (integral) {
        // With integer data and an integer target, an integer water-level
        // solution always exists; snap the floating-point result onto it.
        for (let i = 0; i < seamCount; i += 1) {
            overlaps[i] = Math.round(overlaps[i]);
        }
        let residual = targetOverlap - overlaps.reduce((a, b) => a + b, 0);
        while (residual !== 0) {
            const step = residual > 0 ? 1 : -1;
            let adjusted = false;
            for (let i = 0; i < seamCount; i += 1) {
                if (residual === 0) {
                    break;
                }
                const next = overlaps[i] + step;
                if (next >= minOverlap && next <= caps[i]) {
                    overlaps[i] = next;
                    residual -= step;
                    adjusted = true;
                }
            }
            if (!adjusted) {
                throw new Error('MATH assembly overlap adjustment has no legal seam left.');
            }
        }
    }
    return overlaps;
}

// Build the concrete part placement (offsets from 0 along the growth
// direction) for an already chosen expanded part sequence.
function placeParts(parts, minOverlap, targetAdvance) {
    const seamCount = parts.length - 1;
    if (seamCount === 0) {
        return {
            overlaps: [],
            advance: parts[0].fullAdvance,
            placed: [{ glyphIndex: parts[0].glyphIndex, offset: 0 }]
        };
    }
    const caps = new Array(seamCount);
    for (let i = 0; i < seamCount; i += 1) {
        caps[i] = Math.min(parts[i].endConnectorLength, parts[i + 1].startConnectorLength);
        if (caps[i] < minOverlap) {
            throw new RangeError('MATH assembly connector overlap is smaller than minConnectorOverlap.');
        }
    }
    const fullAdvanceSum = parts.reduce((sum, part) => sum + part.fullAdvance, 0);
    // Shortest possible length: every seam overlaps as much as allowed.
    const minimumLength = fullAdvanceSum - caps.reduce((a, b) => a + b, 0);
    let overlaps;
    if (minimumLength >= targetAdvance) {
        // The construction cannot get any shorter: keep the maximum overlaps.
        overlaps = caps.slice();
    } else {
        // Grow to exactly the target by lowering the common overlap water level.
        const targetOverlap = fullAdvanceSum - targetAdvance;
        overlaps = waterFillOverlaps(caps, minOverlap, targetOverlap);
    }
    const placed = new Array(parts.length);
    let offset = 0;
    for (let i = 0; i < parts.length; i += 1) {
        placed[i] = { glyphIndex: parts[i].glyphIndex, offset: offset };
        if (i < seamCount) {
            offset += parts[i].fullAdvance - overlaps[i];
        }
    }
    const last = parts.length - 1;
    return {
        overlaps: overlaps,
        advance: placed[last].offset + parts[last].fullAdvance,
        placed: placed
    };
}

// Choose the smallest extender repetition count that can reach the target.
function planAssembly(construction, minOverlap, targetAdvance) {
    const assembly = construction.glyphAssembly;
    const baseParts = assembly.parts;
    const hasExtender = baseParts.some(part => part.flags & 1);
    let previousMaximum = -1;
    for (let repeat = 0; repeat <= MAX_REPEAT; repeat += 1) {
        const parts = expandParts(baseParts, repeat);
        // r = 0 of an extender-only assembly leaves an empty sequence;
        // that is not a viable construction.
        if (parts.length === 0) {
            if (!hasExtender) {
                throw new RangeError('MATH assembly contains no usable parts.');
            }
            continue;
        }
        let maximumLength;
        if (parts.length === 1) {
            maximumLength = parts[0].fullAdvance;
        } else {
            for (let i = 0; i < parts.length - 1; i += 1) {
                const cap = Math.min(parts[i].endConnectorLength, parts[i + 1].startConnectorLength);
                if (cap < minOverlap) {
                    throw new RangeError(
                        'MATH assembly connector overlap is smaller than minConnectorOverlap.');
                }
            }
            const fullAdvanceSum = parts.reduce((sum, part) => sum + part.fullAdvance, 0);
            // Largest length uses the minimum legal overlap on every seam.
            maximumLength = fullAdvanceSum - (parts.length - 1) * minOverlap;
        }
        if (maximumLength >= targetAdvance) {
            const placement = placeParts(parts, minOverlap, targetAdvance);
            return {
                kind: 'assembly',
                advance: placement.advance,
                italicsCorrection: assembly.italicsCorrection,
                parts: placement.placed,
                repeat: repeat
            };
        }
        if (!hasExtender) {
            // A fixed part sequence cannot grow any further.
            throw new RangeError('MATH assembly cannot stretch to the requested advance.');
        }
        if (maximumLength <= previousMaximum) {
            // Adding extenders does not increase the reachable length;
            // growth is impossible, so looping further would never succeed.
            throw new RangeError('MATH assembly cannot stretch to the requested advance.');
        }
        previousMaximum = maximumLength;
    }
    throw new RangeError('MATH assembly extender repetition limit exceeded.');
}

// Pick the smallest declared variant whose advance reaches the target.
// The records are not assumed to be sorted: ties are broken by glyph index.
function chooseVariant(variants, targetAdvance) {
    let chosen = null;
    for (let i = 0; i < variants.length; i += 1) {
        const variant = variants[i];
        if (variant.advance < targetAdvance) {
            continue;
        }
        if (chosen === null ||
            variant.advance < chosen.advance ||
            (variant.advance === chosen.advance && variant.glyphIndex < chosen.glyphIndex)) {
            chosen = variant;
        }
    }
    return chosen;
}

/**
 * Compute a stretch plan for a glyph, entirely in font design units.
 * Returns null when the font has no MATH data or the glyph is not covered
 * for the requested direction; throws RangeError for bad arguments or when
 * no variant/assembly can reach the target.
 */
function planStretch(font, glyphIndex, direction, targetAdvance) {
    validateArguments(glyphIndex, direction, targetAdvance, font.numGlyphs);
    const math = font.tables && font.tables.math;
    if (!math || !math.mathVariants) {
        return null;
    }
    const subtable = direction === VERTICAL ?
        math.mathVariants.vertical : math.mathVariants.horizontal;
    const construction = mathTable.findConstruction(subtable, glyphIndex, font.numGlyphs);
    if (!construction) {
        return null;
    }

    const variant = chooseVariant(construction.mathGlyphVariants, targetAdvance);
    if (variant) {
        return {
            kind: 'variant',
            advance: variant.advance,
            italicsCorrection: 0,
            parts: [{ glyphIndex: variant.glyphIndex, offset: 0 }]
        };
    }

    if (!construction.glyphAssembly) {
        throw new RangeError('No MATH variant or assembly reaches the requested advance.');
    }
    return planAssembly(construction, math.mathVariants.minConnectorOverlap, targetAdvance);
}

/**
 * Render a stretch plan as a single Path through Glyph.getPath().
 * Horizontal offsets grow to the right; vertical font offsets grow upward,
 * so they are flipped to downward screen offsets exactly like Glyph.getPath.
 * The italics correction is returned with the plan only and never moves parts.
 */
function buildStretchPath(font, plan, direction, x, y, fontSize) {
    x = x !== undefined ? x : 0;
    y = y !== undefined ? y : 0;
    fontSize = fontSize !== undefined ? fontSize : 72;
    // Match Glyph.getPath's exact operation order so part origins and the
    // glyphs' own control points share one scale in floating point.
    const scale = 1 / font.unitsPerEm * fontSize;
    const fullPath = new Path();
    fullPath.unitsPerEm = font.unitsPerEm;
    const options = { hinting: false, drawLayers: false, drawSVG: false };
    for (let i = 0; i < plan.parts.length; i += 1) {
        const part = plan.parts[i];
        const glyph = font.glyphs.get(part.glyphIndex);
        check.argument(!!glyph, 'MATH stretch part refers to a missing glyph.');
        let partX;
        let partY;
        if (direction === HORIZONTAL) {
            partX = x + part.offset * scale;
            partY = y;
        } else {
            partX = x;
            partY = y - part.offset * scale;
        }
        const partPath = glyph.getPath(partX, partY, fontSize, options, font);
        fullPath.extend(partPath);
    }
    return fullPath;
}

export { planStretch, buildStretchPath, HORIZONTAL, VERTICAL };
