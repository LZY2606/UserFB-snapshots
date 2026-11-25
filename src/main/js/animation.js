/*
 * Copyright (c) Sandflow Consulting LLC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * Offline support for a restricted subset of TTML2 inline <animate> elements.
 * The module is deliberately independent of the browser animation machinery:
 * an animation is resolved against the document clock and sampled directly by
 * the ISD generation code.
 *
 * Supported subset:
 * - hosts: region, body, div, p and span
 * - target properties: tts:opacity (region only) and tts:fontSize
 *   (p/span, and body/div as inheritance sources)
 * - calcMode: "linear" (default) and "discrete"
 * - finite repeatCount in (0, 1024], explicit begin/end/dur time expressions,
 *   fill "remove" (default) or "freeze"
 *
 * @module imscAnimation
 */

import { reportFatal } from "./error.js";
import { ns_tts } from "./names.js";
import { byName } from "./styles.js";
import { hasOwnProperty, parseLength, parseTimeExpression } from "./utils.js";

const OPACITY_QNAME = byName.opacity.qname;
const FONT_SIZE_QNAME = byName.fontSize.qname;

const HOST_KINDS = {
    region: true,
    body: true,
    div: true,
    p: true,
    span: true,
};

/* attributes of <animate> that this implementation does not support */
const UNSUPPORTED_ANIMATE_ATTRS = ["values", "from", "to", "by", "keySplines", "targetElement"];

/**
 * Parsed representation of an inline <animate> element. Timing fields
 * (begin/end/activeEnd) are resolved in a post-pass once host timing is known.
 */
export class AnimateElement {
    constructor() {
        /** @type {string} qualified name of the targeted style property */
        this.targetQName = null;
        /** @type {"opacity"|"fontSize"} */
        this.targetName = null;
        /** @type {number[]|{value: number, unit: string}[]} key values */
        this.values = null;
        /** @type {?string} common unit of fontSize values ("px" or "%") */
        this.unit = null;
        /** @type {"linear"|"discrete"} */
        this.calcMode = "linear";
        /** @type {number[]} normalized key times, strictly increasing 0..1 */
        this.keyTimes = null;
        /** @type {number} */
        this.repeatCount = 1;
        /** @type {"remove"|"freeze"} */
        this.fill = "remove";

        this.explicit_begin = null;
        this.explicit_end = null;
        this.explicit_dur = null;

        /* resolved timing, in document seconds */
        this.begin = null;
        this.simpleDur = null;
        this.activeEnd = null;
    }
}

/**
 * Parses an <animate> node. Any construct outside the supported subset is a
 * fatal error: unsupported animations are never silently treated as static
 * styling.
 *
 * @param {import("./doc").TTElement} doc
 * @param {import("./parser").Node} node
 * @param {string} hostKind Kind of the parent element
 * @param {import("./error").ErrorHandler} errorHandler
 * @returns {AnimateElement}
 */
export function parseAnimateNode(doc, node, hostKind, errorHandler) {

    if (!HOST_KINDS[hostKind]) {
        reportFatal(errorHandler, "<animate> is only supported on region, body, div, p and span elements");
    }

    const anim = new AnimateElement();

    for (const name of UNSUPPORTED_ANIMATE_ATTRS) {
        if (name in node.attributes) {
            reportFatal(errorHandler, "Unsupported <animate> attribute '" + name + "'");
        }
    }

    /* identify the single targeted style property */

    let targetAttr = null;

    for (const i in node.attributes) {

        const attr = node.attributes[i];

        if (attr.uri !== ns_tts) continue;

        if (attr.local !== "opacity" && attr.local !== "fontSize") {
            reportFatal(errorHandler, "Unsupported animation target property tts:" + attr.local);
        }

        if (targetAttr !== null) {
            reportFatal(errorHandler, "An <animate> element may target a single style property");
        }

        targetAttr = attr;
    }

    if (targetAttr === null) {
        reportFatal(errorHandler, "<animate> element does not specify an animated tts style property");
    }

    anim.targetName = targetAttr.local;
    anim.targetQName = targetAttr.uri + " " + targetAttr.local;

    if (anim.targetName === "opacity") {

        if (hostKind !== "region") {
            reportFatal(errorHandler, "tts:opacity animations are only supported on region elements");
        }

        anim.values = parseOpacityValues(targetAttr.value, errorHandler);

    } else {

        if (hostKind === "region") {
            reportFatal(errorHandler, "tts:fontSize animations are not supported on region elements");
        }

        const parsed = parseFontSizeValues(targetAttr.value, errorHandler);
        anim.values = parsed.values;
        anim.unit = parsed.unit;
    }

    if (anim.values.length < 2) {
        reportFatal(errorHandler, "<animate> value list must contain at least two values");
    }

    /* calcMode */

    if ("calcMode" in node.attributes) {

        const calcMode = node.attributes.calcMode.value;

        if (calcMode !== "linear" && calcMode !== "discrete") {
            reportFatal(errorHandler, "Unsupported calcMode '" + calcMode + "' (only linear and discrete are supported)");
        }

        anim.calcMode = calcMode;
    }

    /* keyTimes */

    if ("keyTimes" in node.attributes) {

        const parts = node.attributes.keyTimes.value.split(";").map(s => s.trim());

        if (parts.length !== anim.values.length) {
            reportFatal(errorHandler, "keyTimes must contain the same number of entries as the animation values");
        }

        const keyTimes = parts.map(s => {
            const v = parseFloat(s);
            if (!Number.isFinite(v)) {
                reportFatal(errorHandler, "Malformed keyTimes entry '" + s + "'");
            }
            return v;
        });

        if (keyTimes[0] !== 0 || keyTimes[keyTimes.length - 1] !== 1) {
            reportFatal(errorHandler, "keyTimes must start at 0 and end at 1");
        }

        for (let i = 1; i < keyTimes.length; i++) {
            if (!(keyTimes[i] > keyTimes[i - 1])) {
                reportFatal(errorHandler, "keyTimes must be strictly increasing");
            }
        }

        anim.keyTimes = keyTimes;

    } else {

        anim.keyTimes = [];

        for (let i = 0; i < anim.values.length; i++) {
            anim.keyTimes.push(i / (anim.values.length - 1));
        }
    }

    /* timing */

    if (!("dur" in node.attributes)) {
        reportFatal(errorHandler, "<animate> element must specify a dur attribute");
    }

    anim.explicit_dur = parseTimeExpression(doc.tickRate, doc.effectiveFrameRate, node.attributes.dur.value);

    if (anim.explicit_dur === null || !(anim.explicit_dur > 0) || !Number.isFinite(anim.explicit_dur)) {
        reportFatal(errorHandler, "<animate> dur must be a positive finite duration");
    }

    if ("begin" in node.attributes) {

        anim.explicit_begin = parseTimeExpression(doc.tickRate, doc.effectiveFrameRate, node.attributes.begin.value);

        if (anim.explicit_begin === null || !Number.isFinite(anim.explicit_begin)) {
            reportFatal(errorHandler, "Malformed <animate> begin value");
        }
    }

    if ("end" in node.attributes) {

        anim.explicit_end = parseTimeExpression(doc.tickRate, doc.effectiveFrameRate, node.attributes.end.value);

        if (anim.explicit_end === null || !Number.isFinite(anim.explicit_end)) {
            reportFatal(errorHandler, "Malformed <animate> end value");
        }
    }

    /* repeatCount */

    if ("repeatCount" in node.attributes) {

        const rc = parseFloat(node.attributes.repeatCount.value);

        if (!Number.isFinite(rc) || !(rc > 0) || rc > 1024) {
            reportFatal(errorHandler, "repeatCount must be a finite number in (0, 1024]");
        }

        anim.repeatCount = rc;
    }

    /* fill */

    if ("fill" in node.attributes) {

        const fill = node.attributes.fill.value;

        if (fill !== "remove" && fill !== "freeze") {
            reportFatal(errorHandler, "Unsupported fill '" + fill + "' (only remove and freeze are supported)");
        }

        anim.fill = fill;
    }

    return anim;
}

function parseOpacityValues(str, errorHandler) {

    const values = str.split(";").map(s => {

        const v = parseFloat(s.trim());

        if (!Number.isFinite(v) || v < 0 || v > 1) {
            reportFatal(errorHandler, "tts:opacity animation values must be numbers in [0,1]");
        }

        return v;
    });

    return values;
}

function parseFontSizeValues(str, errorHandler) {

    const parts = str.split(";").map(s => s.trim());
    const values = [];
    let unit = null;

    for (const part of parts) {

        const l = parseLength(part);

        if (l === null || !(l.value > 0) || !Number.isFinite(l.value) ||
            (l.unit !== "px" && l.unit !== "%")) {
            reportFatal(errorHandler, "tts:fontSize animation values must be single positive px or % lengths");
        }

        if (unit === null) {
            unit = l.unit;
        } else if (l.unit !== unit) {
            reportFatal(errorHandler, "All tts:fontSize animation values must use the same unit");
        }

        values.push({ value: l.value, unit: l.unit });
    }

    return { values, unit };
}

/**
 * Resolves the absolute timing of a single <animate> element against its
 * host. Called from the document timing pass: the host begin has already been
 * resolved at that point. The animation clock starts at the unclipped host
 * begin; ancestor visibility clipping only limits the effective interval and
 * never shifts or rescales the phase.
 *
 * @param {Object} host Host document element
 * @param {AnimateElement} anim
 */
export function resolveAnimateTiming(host, anim) {

    anim.begin = host.begin + (anim.explicit_begin !== null ? anim.explicit_begin : 0);
    anim.simpleDur = anim.explicit_dur;

    const repeatEnd = anim.begin + anim.simpleDur * anim.repeatCount;

    let activeEnd = repeatEnd;

    if (anim.explicit_end !== null) {
        /* explicit end is offset from the host begin, like <set> end */
        activeEnd = Math.min(activeEnd, host.begin + anim.explicit_end);
    }

    anim.activeEnd = activeEnd;

    if (!(anim.activeEnd > anim.begin) || !Number.isFinite(anim.activeEnd)) {
        throw new Error("<animate> active duration must be greater than zero and finite");
    }
}

/**
 * Registers the clipped, finite media time events of every <animate> element
 * in the document: clipped begin, active end, cycle boundaries and keyTimes
 * instants. Must be called after host timing has been resolved.
 *
 * @param {import("./doc").TTElement} doc
 */
export function resolveDocumentAnimations(doc) {

    const regions = doc.head.layout.regions;

    /* region-hosted animations are clipped by the region's own interval */

    for (const id in regions) {
        if (hasOwnProperty(regions, id)) {
            registerHostAnimationEvents(doc, regions[id], regions[id].begin, regions[id].end, regions[id].id);
        }
    }

    /* content animations are clipped by every ancestor and by their
     * associated region; the phase itself is never shifted */

    if (doc.body) {
        registerHostAnimationEvents(doc, doc.body, doc.body.begin, doc.body.end, doc.body.regionID || "");
    }
}

function registerHostAnimationEvents(doc, host, clipBegin, clipEnd, regionID) {

    let begin = clipBegin;
    let end = clipEnd;

    if (hasOwnProperty(host, "begin")) begin = Math.max(begin, host.begin);
    if (hasOwnProperty(host, "end")) end = Math.min(end, host.end);

    let rid = regionID;

    if (hasOwnProperty(host, "regionID") && host.regionID) rid = host.regionID;

    if (rid && hasOwnProperty(doc.head.layout.regions, rid)) {

        const region = doc.head.layout.regions[rid];

        begin = Math.max(begin, region.begin);
        end = Math.min(end, region.end);
    }

    if (host.animates) {

        for (const anim of host.animates) {
            registerAnimationEvents(doc, anim, begin, end);
        }
    }

    if (host.contents) {
        for (const child of host.contents) {
            if (child && typeof child === "object") {
                registerHostAnimationEvents(doc, child, begin, end, rid);
            }
        }
    }
}

function registerAnimationEvents(doc, anim, clipBegin, clipEnd) {

    /* ancestor visibility clips only limit the effective interval: the phase
     * is neither shifted nor rescaled, only the registered events are clipped */

    const lo = Math.max(anim.begin, clipBegin);
    const hi = Math.min(anim.activeEnd, Number.isFinite(clipEnd) ? clipEnd : anim.activeEnd);

    if (!(hi > lo)) return;

    insertEvent(doc, lo);
    insertEvent(doc, hi);

    const dur = anim.simpleDur;

    /* cycle boundaries strictly inside the clipped effective interval */

    const maxCycle = Math.floor((anim.activeEnd - anim.begin) / dur);

    for (let k = 1; k <= maxCycle; k++) {

        const t = anim.begin + k * dur;

        if (t >= anim.activeEnd) break;

        insertEvent(doc, t, lo, hi);
    }

    /* keyTimes instants for each (possibly partial) cycle, clipped to the
     * effective interval */

    const cycleCount = Math.ceil((anim.activeEnd - anim.begin) / dur);

    for (let k = 0; k < cycleCount; k++) {

        const cycleStart = anim.begin + k * dur;

        if (cycleStart >= anim.activeEnd) break;

        for (const kt of anim.keyTimes) {

            /* keyTimes 0 and 1 coincide with cycle boundaries */
            if (kt === 0 || kt === 1) continue;

            const t = cycleStart + kt * dur;

            if (t >= anim.activeEnd) break;

            insertEvent(doc, t, lo, hi);
        }
    }
}

function insertEvent(doc, t, lowerBound, upperBound) {

    if (!Number.isFinite(t)) return;

    /* drop instants outside the clipped effective interval, unless no bounds
     * were supplied (the clipped begin/end always register unconditionally) */

    if (lowerBound !== undefined && (t < lowerBound || t >= upperBound)) return;

    const events = doc.events;
    let min = 0;
    let max = events.length - 1;

    while (min <= max) {

        const cur = Math.floor((min + max) / 2);

        if (events[cur] < t) {
            min = cur + 1;
        } else if (events[cur] > t) {
            max = cur - 1;
        } else {
            return; /* already present */
        }
    }

    events.splice(min, 0, t);
}

/**
 * Samples an animation at an absolute document time.
 *
 * @param {AnimateElement} anim
 * @param {number} offset Document time in seconds
 * @returns {number|{value: number, unit: string}|undefined} The animated
 *          value, or undefined when the animation does not apply (before the
 *          begin or after a remove fill).
 */
export function sampleAnimation(anim, offset) {

    if (offset < anim.begin) return undefined;

    if (offset < anim.activeEnd) {

        const elapsed = offset - anim.begin;
        const local = elapsed % anim.simpleDur; /* new cycle restarts at the first value */
        const progress = local / anim.simpleDur;

        return valueAtProgress(anim, progress);
    }

    if (anim.fill !== "freeze") return undefined;

    /* freeze: hold the left-limit value at the active end. A whole number of
     * cycles freezes at the last value, a partial cycle at its true progress. */

    const elapsed = anim.activeEnd - anim.begin;
    const cycles = elapsed / anim.simpleDur;
    const fraction = cycles - Math.floor(cycles);

    const progress = fraction === 0 ? 1 : fraction;

    return valueAtProgress(anim, progress);
}

function valueAtProgress(anim, progress) {

    const keyTimes = anim.keyTimes;
    const values = anim.values;

    if (anim.calcMode === "discrete") {

        let index = 0;

        for (let i = 0; i < keyTimes.length; i++) {
            if (keyTimes[i] <= progress) index = i;
            else break;
        }

        return values[index];
    }

    /* linear */

    if (progress >= 1) return values[values.length - 1];

    let i = 0;

    while (i < keyTimes.length - 1 && keyTimes[i + 1] < progress) i++;

    const span = keyTimes[i + 1] - keyTimes[i];
    const f = span === 0 ? 0 : (progress - keyTimes[i]) / span;

    if (anim.targetName === "opacity") {

        return values[i] + f * (values[i + 1] - values[i]);
    }

    /* fontSize: interpolate the numeric component, keep the common unit */

    return {
        value: values[i].value + f * (values[i + 1].value - values[i].value),
        unit: anim.unit,
    };
}

/**
 * Applies the animations of a host element to its working style attributes.
 * <set> styling is applied first; active animations are then applied in XML
 * document order, so a later animation overrides an earlier one for the same
 * property. The host document and previously computed values are never
 * mutated.
 *
 * @param {Object} host Document element carrying an `animates` array
 * @param {Record<string, any>} styleAttrs Mutable ISD style attribute map
 * @param {number} offset Document time in seconds
 */
export function applyAnimations(host, styleAttrs, offset) {

    if (!host.animates) return;

    for (const anim of host.animates) {

        const value = sampleAnimation(anim, offset);

        if (value !== undefined) {
            styleAttrs[anim.targetQName] = value;
        }
    }
}

/**
 * Computes the union of the intervals during which linear animations actually
 * take effect, clipped by the host element and all of its ancestors (including
 * the associated region). Overlapping or adjacent intervals are merged.
 * Discrete animations and freeze tails are excluded.
 *
 * @param {import("./doc").TTElement} doc
 * @returns {[number, number][]} Fresh array of independent [start, end] pairs
 */
export function collectContinuousAnimationRanges(doc) {

    const ranges = [];
    const regions = doc.head.layout.regions;

    for (const id in regions) {
        if (hasOwnProperty(regions, id)) {
            collectFromHost(doc, regions[id], regions[id].begin, regions[id].end, regions[id].id, ranges);
        }
    }

    if (doc.body) {
        collectFromHost(doc, doc.body, doc.body.begin, doc.body.end, doc.body.regionID || "", ranges);
    }

    if (ranges.length === 0) return [];

    ranges.sort((a, b) => a[0] - b[0] || a[1] - b[1]);

    const merged = [];

    for (const range of ranges) {

        const last = merged[merged.length - 1];

        if (last !== undefined && range[0] <= last[1]) {
            last[1] = Math.max(last[1], range[1]);
        } else {
            merged.push([range[0], range[1]]);
        }
    }

    return merged;
}

function collectFromHost(doc, host, clipBegin, clipEnd, regionID, ranges) {

    let begin = clipBegin;
    let end = clipEnd;

    if (hasOwnProperty(host, "begin")) begin = Math.max(begin, host.begin);
    if (hasOwnProperty(host, "end")) end = Math.min(end, host.end);

    let rid = regionID;

    if (hasOwnProperty(host, "regionID") && host.regionID) rid = host.regionID;

    if (rid && regions_has(doc, rid)) {

        const region = doc.head.layout.regions[rid];

        begin = Math.max(begin, region.begin);
        end = Math.min(end, region.end);
    }

    if (host.animates) {

        for (const anim of host.animates) {

            if (anim.calcMode !== "linear") continue;

            /* freeze tails are not continuous animation intervals */

            const lo = Math.max(anim.begin, begin);
            const hi = Math.min(anim.activeEnd, end);

            if (Number.isFinite(lo) && Number.isFinite(hi) && hi > lo) {
                ranges.push([lo, hi]);
            }
        }
    }

    if (host.contents) {

        for (const child of host.contents) {
            if (child && typeof child === "object") {
                collectFromHost(doc, child, begin, end, rid, ranges);
            }
        }
    }
}

function regions_has(doc, id) {
    return hasOwnProperty(doc.head.layout.regions, id);
}
