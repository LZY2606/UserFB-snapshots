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
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
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
 * Limited offline TTML2 inline `<animate>` support.
 *
 * The implementation intentionally has no DOM or browser animation dependency.
 * It parses the small supported subset, resolves its SMIL-like timing, evaluates
 * values at arbitrary media times, and reports sampling events and continuous
 * intervals.
 *
 * @module imscAnimation
 */

import { reportFatal } from "./error.js";
import { ns_tts } from "./names.js";
import { parseLength } from "./utils.js";

export const OPACITY_QNAME = ns_tts + " opacity";
export const FONT_SIZE_QNAME = ns_tts + " fontSize";

const EPS = 1e-12;

const SUPPORTED_HOSTS = new Set(["region", "body", "div", "p", "span"]);
const SUPPORTED_PLAIN_ATTRIBUTES = new Set([
    "begin",
    "dur",
    "end",
    "repeatCount",
    "fill",
    "calcMode",
    "keyTimes",
]);

const DECIMAL_RE = /^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?$/;

/**
 * Parsed inline animation. Timing fields use the same absolute media-time
 * convention as the rest of the document model.
 */
export class AnimateElement {
    constructor() {
        this.kind = "animate";
        this.targetQName = null;
        this.valueType = null;
        this.values = [];
        this.calcMode = "linear";
        this.keyTimes = [0, 1];
        this.repeatCount = 1;
        this.fill = "remove";
        this.simpleDuration = null;

        this.explicitBegin = null;
        this.explicitEnd = null;
        this.explicitDur = null;

        this.begin = 0;
        this.end = 0;
    }
}

function animationError(errorHandler, message) {
    reportFatal(errorHandler, "Unsupported or invalid TTML animate: " + message);
}

function getAttribute(node, uri, localName) {
    if (!node || !node.attributes) return null;

    for (const key in node.attributes) {
        if (!Object.prototype.hasOwnProperty.call(node.attributes, key)) continue;

        const attr = node.attributes[key];

        if (attr && attr.uri === uri && attr.local === localName) {
            return attr.value;
        }
    }

    return null;
}

function getPlainAttribute(node, name) {
    if (!node || !node.attributes) return null;

    const attr = node.attributes[name];

    return attr && Object.prototype.hasOwnProperty.call(attr, "value") ? attr.value : null;
}

function parseDecimal(raw) {
    if (typeof raw !== "string") return null;

    const value = raw.trim();

    if (!DECIMAL_RE.test(value)) return null;

    const n = Number(value);

    return Number.isFinite(n) ? n : null;
}

function splitValueList(raw) {
    return raw.split(";").map((item) => item.trim());
}

function parseOpacityValues(raw, errorHandler) {
    const tokens = splitValueList(raw);

    if (tokens.length < 2) {
        animationError(errorHandler, "tts:opacity must contain at least two values");
    }

    return tokens.map((token) => {
        const value = parseDecimal(token);

        if (value === null || value < 0 || value > 1) {
            animationError(errorHandler, "tts:opacity values must be finite numbers in [0,1]");
        }

        return value;
    });
}

function parseFontSizeValues(raw, errorHandler) {
    const tokens = splitValueList(raw);

    if (tokens.length < 2) {
        animationError(errorHandler, "tts:fontSize must contain at least two values");
    }

    let unit = null;

    const values = tokens.map((token) => {
        const length = parseLength(token);

        if (!length ||
            (length.unit !== "px" && length.unit !== "%") ||
            !Number.isFinite(length.value) ||
            length.value <= 0) {
            animationError(
                errorHandler,
                "tts:fontSize values must each be one positive px or % length",
            );
        }

        if (unit === null) {
            unit = length.unit;
        } else if (unit !== length.unit) {
            animationError(errorHandler, "all tts:fontSize values must use the same px or % unit");
        }

        return { value: length.value, unit: length.unit };
    });

    return values;
}

function parseKeyTimes(raw, valueCount, errorHandler) {
    if (raw === null) {
        const result = [];

        for (let i = 0; i < valueCount; i++) {
            result.push(valueCount === 1 ? 0 : i / (valueCount - 1));
        }

        return result;
    }

    const tokens = splitValueList(raw);

    if (tokens.length !== valueCount) {
        animationError(errorHandler, "keyTimes must have the same number of entries as values");
    }

    const keyTimes = tokens.map((token) => {
        const value = parseDecimal(token);

        if (value === null || value < 0 || value > 1) {
            animationError(errorHandler, "keyTimes must be finite numbers from 0 to 1");
        }

        return value;
    });

    if (keyTimes[0] !== 0 || keyTimes[keyTimes.length - 1] !== 1) {
        animationError(errorHandler, "keyTimes must start at 0 and end at 1");
    }

    for (let i = 1; i < keyTimes.length; i++) {
        if (!(keyTimes[i] > keyTimes[i - 1])) {
            animationError(errorHandler, "keyTimes must be strictly increasing");
        }
    }

    return keyTimes;
}

/**
 * Creates and validates the supported animate subset from a SAX attribute node.
 *
 * @param {Object} node Parser node
 * @param {string} hostKind kind of the direct host element
 * @param {{explicit_begin: ?number, explicit_end: ?number, explicit_dur: ?number}} timing
 * @param {Function} errorHandler
 * @returns {AnimateElement}
 */
export function createAnimateElement(node, hostKind, timing, errorHandler) {
    if (!SUPPORTED_HOSTS.has(hostKind)) {
        animationError(errorHandler, "<animate> is only supported on region, body, div, p and span");
    }

    const anim = new AnimateElement();

    anim.explicitBegin = timing.explicit_begin;
    anim.explicitEnd = timing.explicit_end;
    anim.explicitDur = timing.explicit_dur;

    const opacityRaw = getAttribute(node, ns_tts, "opacity");
    const fontSizeRaw = getAttribute(node, ns_tts, "fontSize");

    for (const key in node.attributes) {
        if (!Object.prototype.hasOwnProperty.call(node.attributes, key)) continue;

        const attr = node.attributes[key];

        if (attr && attr.uri === ns_tts &&
            attr.local !== "opacity" && attr.local !== "fontSize") {
            animationError(errorHandler, "animated tts:" + attr.local + " is not supported");
        }
    }

    if (opacityRaw !== null && fontSizeRaw !== null) {
        animationError(errorHandler, "one <animate> may target only one style property");
    }

    if (opacityRaw === null && fontSizeRaw === null) {
        animationError(errorHandler, "animate must specify a supported tts:opacity or tts:fontSize value list");
    }

    if (opacityRaw !== null) {
        if (hostKind !== "region") {
            animationError(errorHandler, "tts:opacity animation is supported only on region");
        }

        anim.targetQName = OPACITY_QNAME;
        anim.valueType = "number";
        anim.values = parseOpacityValues(opacityRaw, errorHandler);
    } else {
        if (hostKind !== "p" && hostKind !== "span" && hostKind !== "body" && hostKind !== "div") {
            animationError(
                errorHandler,
                "tts:fontSize animation is supported on p and span, and on body/div as inheritance sources",
            );
        }

        anim.targetQName = FONT_SIZE_QNAME;
        anim.valueType = "length";
        anim.values = parseFontSizeValues(fontSizeRaw, errorHandler);
    }

    for (const key in node.attributes) {
        if (!Object.prototype.hasOwnProperty.call(node.attributes, key)) continue;

        const attr = node.attributes[key];

        if (!attr) continue;

        if (attr.uri === ns_tts) {
            if (attr.local !== "opacity" && attr.local !== "fontSize") {
                animationError(errorHandler, "animated tts:" + attr.local + " is not supported");
            }

            continue;
        }

        if (attr.local === "href" || attr.local === "src") {
            animationError(errorHandler, "external/out-of-line animation is not supported");
        }

        /* xml:* and other namespaced metadata attributes do not affect playback. */
        if (attr.uri !== "" && attr.uri !== null && attr.uri !== undefined) {
            continue;
        }

        if (!SUPPORTED_PLAIN_ATTRIBUTES.has(attr.local)) {
            animationError(errorHandler, "attribute " + attr.local + " is not supported");
        }
    }

    const beginRaw = getPlainAttribute(node, "begin");

    if (beginRaw !== null && anim.explicitBegin === null) {
        animationError(errorHandler, "malformed begin time expression");
    }

    const endRaw = getPlainAttribute(node, "end");

    if (endRaw !== null && anim.explicitEnd === null) {
        animationError(errorHandler, "malformed end time expression");
    }

    const durRaw = getPlainAttribute(node, "dur");

    if (durRaw === null || anim.explicitDur === null ||
        !Number.isFinite(anim.explicitDur) || anim.explicitDur <= 0) {
        animationError(errorHandler, "dur must be a positive finite time expression");
    }

    anim.simpleDuration = anim.explicitDur;

    const repeatRaw = getPlainAttribute(node, "repeatCount");

    if (repeatRaw !== null) {
        const repeatCount = parseDecimal(repeatRaw);

        if (repeatCount === null ||
            !Number.isFinite(repeatCount) ||
            repeatCount <= 0 ||
            repeatCount > 1024) {
            animationError(errorHandler, "repeatCount must be a finite number in (0,1024]");
        }

        anim.repeatCount = repeatCount;
    }

    const fillRaw = getPlainAttribute(node, "fill");

    if (fillRaw !== null) {
        if (fillRaw !== "remove" && fillRaw !== "freeze") {
            animationError(errorHandler, "fill must be remove or freeze in this subset");
        }

        anim.fill = fillRaw;
    }

    const calcModeRaw = getPlainAttribute(node, "calcMode");

    if (calcModeRaw !== null) {
        if (calcModeRaw === "linear" || calcModeRaw === "discrete") {
            anim.calcMode = calcModeRaw;
        } else if (calcModeRaw === "paced" || calcModeRaw === "spline") {
            animationError(errorHandler, "calcMode " + calcModeRaw + " is not supported");
        } else {
            animationError(errorHandler, "calcMode must be linear or discrete");
        }
    }

    anim.keyTimes = parseKeyTimes(
        getPlainAttribute(node, "keyTimes"),
        anim.values.length,
        errorHandler,
    );

    return anim;
}

/**
 * Resolves absolute begin/end times. The active end is
 * `min(begin + dur * repeatCount, implicitBegin + explicitEnd)`, matching the
 * existing convention that an explicit `end` is relative to the parent's
 * implicit begin.
 */
export function resolveAnimateTiming(anim, prevSibling, parent) {
    const isInSeq = parent && parent.timeContainer === "seq";

    let implicitBegin = 0;

    if (parent) {
        if (isInSeq && prevSibling) {
            implicitBegin = prevSibling.end;
        } else {
            implicitBegin = parent.begin;
        }
    }

    anim.implicitBegin = implicitBegin;
    anim.begin = anim.explicitBegin !== null ?
        anim.explicitBegin + implicitBegin :
        implicitBegin;

    let activeEnd = anim.begin + anim.simpleDuration * anim.repeatCount;

    if (anim.explicitEnd !== null) {
        activeEnd = Math.min(activeEnd, implicitBegin + anim.explicitEnd);
    }

    anim.end = activeEnd;

    return anim;
}

function snapUnitInterval(u) {
    if (u < EPS) return 0;
    if (u > 1 - EPS) return 1;
    return u;
}

function discreteValueIndex(anim, u, leftLimit) {
    const kt = anim.keyTimes;

    if (leftLimit) {
        for (let j = 1; j < kt.length; j++) {
            if (Math.abs(u - kt[j]) <= EPS) {
                return j - 1;
            }
        }
    }

    let index = 0;

    for (let j = 0; j < kt.length; j++) {
        if (u + EPS >= kt[j]) index = j;
    }

    return Math.min(index, anim.values.length - 1);
}

function interpolateValue(anim, index, alpha) {
    const a = anim.values[index];
    const b = anim.values[index + 1];

    if (anim.valueType === "number") {
        return a + (b - a) * alpha;
    }

    return {
        value: a.value + (b.value - a.value) * alpha,
        unit: a.unit,
    };
}

function sampleAtUnitProgress(anim, rawU, leftLimit = false) {
    let u = snapUnitInterval(rawU);

    if (anim.calcMode === "discrete") {
        return anim.values[discreteValueIndex(anim, u, leftLimit)];
    }

    if (u <= 0) return anim.values[0];
    if (u >= 1) return anim.values[anim.values.length - 1];

    const kt = anim.keyTimes;
    let i = 0;

    while (i < kt.length - 2 && u > kt[i + 1]) i++;

    const k0 = kt[i];
    const k1 = kt[i + 1];
    let alpha = (u - k0) / (k1 - k0);

    if (!Number.isFinite(alpha)) alpha = 0;
    if (alpha < 0) alpha = 0;
    if (alpha > 1) alpha = 1;

    return interpolateValue(anim, i, alpha);
}

function sampleActive(anim, time) {
    const x = (time - anim.begin) / anim.simpleDuration;
    let cycle = Math.floor(x);
    let u = x - cycle;

    /* Recover exact floating-point boundaries and make a new cycle start at
       its first value. */
    if (u >= 1 - EPS) {
        cycle += 1;
        u = 0;
    }

    if (u <= EPS) u = 0;

    return sampleAtUnitProgress(anim, u, false);
}

function sampleFreeze(anim) {
    const activeDuration = anim.end - anim.begin;
    let progress = activeDuration / anim.simpleDuration;

    if (!Number.isFinite(progress) || progress <= 0) {
        return anim.values[0];
    }

    progress = Math.min(progress, anim.repeatCount);

    const nearestInteger = Math.round(progress);

    if (Math.abs(progress - nearestInteger) <= EPS) {
        return anim.values[anim.values.length - 1];
    }

    const u = progress - Math.floor(progress);

    /* Freeze is the left limit at the active endpoint. This matters for
       discrete animations whose endpoint is also an interior keyTime. */
    return sampleAtUnitProgress(anim, u, true);
}

/**
 * Evaluates an animation for an ISD offset.
 *
 * @returns {number|{value:number,unit:string}|undefined} undefined when the
 * animation does not apply at `time`.
 */
export function evaluateAnimate(anim, time, hostEnd = Number.POSITIVE_INFINITY) {
    if (!anim || !Number.isFinite(time)) return undefined;

    if (time < anim.begin) return undefined;

    if (time < anim.end) {
        return sampleActive(anim, time);
    }

    if (anim.fill === "freeze" && time < hostEnd) {
        return sampleFreeze(anim);
    }

    return undefined;
}

/**
 * All cycle and keyTime instants in an animation's own active interval.
 */
export function getAnimateScheduleTimes(anim) {
    const times = [anim.begin, anim.end];
    const duration = anim.simpleDuration;

    if (!(duration > 0) || !(anim.end > anim.begin)) return times;

    const effectiveCycles = (anim.end - anim.begin) / duration;
    const maxCycle = Math.floor(effectiveCycles + EPS);

    for (let cycle = 0; cycle <= maxCycle; cycle++) {
        const cycleBegin = anim.begin + cycle * duration;

        if (cycleBegin <= anim.end + EPS) {
            times.push(cycleBegin);
        }

        for (const keyTime of anim.keyTimes) {
            const t = anim.begin + (cycle + keyTime) * duration;

            if (t >= anim.begin - EPS && t <= anim.end + EPS) {
                times.push(t);
            }
        }
    }

    return times;
}

function inheritedRegionID(element, currentRegionID) {
    if (element && "regionID" in element && element.regionID) {
        return element.regionID;
    }

    return currentRegionID;
}

/**
 * Enumerates animation hosts with their timed XML ancestors and associated
 * region. The arrays are internal-looking but contain no copied time state.
 */
export function collectAnimationHostRecords(doc) {
    const records = [];

    if (!doc || !doc.head || !doc.head.layout || !doc.head.layout.regions) {
        return records;
    }

    const regions = doc.head.layout.regions;

    for (const id in regions) {
        if (!Object.prototype.hasOwnProperty.call(regions, id)) continue;

        const region = regions[id];

        if (region && Array.isArray(region.animations) && region.animations.length > 0) {
            records.push({ host: region, ancestors: [], region: null });
        }
    }

    const visit = (element, ancestors, regionID) => {
        if (!element) return;

        const currentRegionID = inheritedRegionID(element, regionID);
        const region = currentRegionID in regions ? regions[currentRegionID] : null;

        if (Array.isArray(element.animations) && element.animations.length > 0) {
            records.push({ host: element, ancestors: ancestors.slice(), region });
        }

        if (Array.isArray(element.contents)) {
            for (const child of element.contents) {
                visit(child, ancestors.concat(element), currentRegionID);
            }
        }
    };

    if (doc.body) visit(doc.body, [], "");

    return records;
}

function getVisibleClip(record) {
    let start = Number.NEGATIVE_INFINITY;
    let end = Number.POSITIVE_INFINITY;

    const elements = [record.host, ...record.ancestors];

    if (record.region) elements.push(record.region);

    for (const element of elements) {
        if (!element) continue;

        if (Number.isFinite(element.begin)) {
            start = Math.max(start, element.begin);
        }

        if (element.end !== undefined && element.end !== null) {
            end = Math.min(end, element.end);
        }
    }

    if (!Number.isFinite(start)) start = 0;

    return { start, end };
}

/**
 * Animation sampling events after clipping to host, ancestor and region
 * visibility intervals.
 */
export function collectAnimationEventTimes(doc) {
    const times = [];

    for (const record of collectAnimationHostRecords(doc)) {
        const clip = getVisibleClip(record);

        for (const anim of record.host.animations) {
            const start = Math.max(anim.begin, clip.start);
            const end = Math.min(anim.end, clip.end);

            if (Number.isFinite(start) && Number.isFinite(end) && start <= end) {
                times.push(start);
                times.push(end);

                for (const t of getAnimateScheduleTimes(anim)) {
                    if (t >= start - EPS && t <= end + EPS) {
                        times.push(t);
                    }
                }
            }
        }
    }

    return times;
}

/**
 * Merges document timing events with animation events. A fresh array is
 * returned so callers cannot mutate document state through the result.
 */
export function getMediaTimeEventsWithAnimations(doc) {
    const times = Array.isArray(doc.events) ? doc.events.slice() : [];

    for (const t of collectAnimationEventTimes(doc)) {
        if (Number.isFinite(t)) times.push(t);
    }

    times.sort((a, b) => a - b);

    const result = [];

    for (const t of times) {
        if (!Number.isFinite(t)) continue;

        const last = result[result.length - 1];

        if (last === undefined || Math.abs(t - last) > EPS) {
            result.push(t);
        }
    }

    return result;
}

/**
 * Union of active linear-animation intervals, clipped by all visible timing
 * ancestors. Freeze tails and discrete animations do not contribute.
 */
export function collectContinuousAnimationRanges(doc) {
    const ranges = [];

    for (const record of collectAnimationHostRecords(doc)) {
        const clip = getVisibleClip(record);

        for (const anim of record.host.animations) {
            if (anim.calcMode !== "linear") continue;

            const start = Math.max(anim.begin, clip.start);
            const end = Math.min(anim.end, clip.end);

            if (Number.isFinite(start) && Number.isFinite(end) && end > start) {
                ranges.push([start, end]);
            }
        }
    }

    ranges.sort((a, b) => (a[0] - b[0]) || (a[1] - b[1]));

    const merged = [];

    for (const range of ranges) {
        const start = range[0];
        const end = range[1];
        const last = merged[merged.length - 1];

        if (!last || start > last[1] + EPS) {
            merged.push([start, end]);
        } else if (end > last[1]) {
            last[1] = end;
        }
    }

    /* Return independent arrays even on repeated calls. */
    return merged.map((range) => [range[0], range[1]]);
}
