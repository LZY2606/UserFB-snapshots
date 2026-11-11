/*
 * interpolates between a set of colors uzing a bezier spline
 * blend mode formulas taken from https://web.archive.org/web/20180110014946/http://www.venture-ware.com/kevin/coding/lets-learn-math-photoshop-blend-modes/
 */

import '../io/rgb/index.js';
import chroma from '../chroma.js';
import limit from '../utils/limit.js';

const { min, max, pow } = Math;

const blend = (bottom, top, mode, options) => {
    if (!blend[mode]) {
        throw new Error('unknown blend mode ' + mode);
    }
    return blend[mode](bottom, top, options);
};

const blend_f = (f) => (bottom, top) => {
    const c0 = chroma(top).rgba();
    const c1 = chroma(bottom).rgba();
    c0[3] = limit(c0[3], 0, 1);
    c1[3] = limit(c1[3], 0, 1);
    const alpha = c0[3] + c1[3] * (1 - c0[3]);
    if (alpha === 0) return chroma.rgb(0, 0, 0, 0);
    if (c0[3] === 0) return chroma.rgb(c1);
    if (c1[3] === 0) return chroma.rgb(c0);

    const out = f(c0, c1);
    if (c0[3] === 1 && c1[3] === 1) return chroma.rgb(out);

    // Source-over composition: blend only where source and backdrop overlap.
    for (let i = 0; i < 3; i++) {
        out[i] =
            (c0[3] * (1 - c1[3]) * c0[i] +
                c0[3] * c1[3] * limit(out[i], 0, 255) +
                (1 - c0[3]) * c1[3] * c1[i]) /
            alpha;
    }
    return chroma.rgb(...out, alpha);
};

const each = (f) => (c0, c1) => {
    const out = [];
    out[0] = f(c0[0], c1[0]);
    out[1] = f(c0[1], c1[1]);
    out[2] = f(c0[2], c1[2]);
    return out;
};

const normal = (a) => a;
const multiply = (a, b) => (a * b) / 255;
const darken = (a, b) => (a > b ? b : a);
const lighten = (a, b) => (a > b ? a : b);
const screen = (a, b) => 255 * (1 - (1 - a / 255) * (1 - b / 255));
const overlay = (a, b) =>
    b < 128 ? (2 * a * b) / 255 : 255 * (1 - 2 * (1 - a / 255) * (1 - b / 255));
const burn = (a, b) =>
    b === 255 ? 255 : 255 * (1 - (1 - b / 255) / (a / 255));
const dodge = (a, b) => {
    if (a === 255) return 255;
    a = (255 * (b / 255)) / (1 - a / 255);
    return a > 255 ? 255 : a;
};

// # add = (a,b) ->
// #     if (a + b > 255) then 255 else a + b

// sRGB transfer function (IEC 61966-2-1), operating on [0, 255] encoded channels
const srgbDecode = (c) => {
    c = c / 255;
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
};

const srgbEncode = (l) =>
    (l <= 0.0031308 ? 12.92 * l : 1.055 * pow(l, 1 / 2.4) - 0.055) * 255;

// linear-light formulas: cs is the source (top) and cb is the backdrop (bottom),
// both in linear-light sRGB [0, 1]
const linearNormal = (cs) => cs;
const linearMultiply = (cs, cb) => cs * cb;
const linearScreen = (cs, cb) => 1 - (1 - cb) * (1 - cs);
const linearDarken = (cs, cb) => min(cs, cb);
const linearLighten = (cs, cb) => max(cs, cb);
const linearOverlay = (cs, cb) =>
    cb <= 0.5 ? 2 * cb * cs : 1 - 2 * (1 - cb) * (1 - cs);
const linearDodge = (cs, cb) => {
    if (cb === 0) return 0;
    if (cs === 1) return 1;
    return min(1, cb / (1 - cs));
};
const linearBurn = (cs, cb) => {
    if (cb === 1) return 1;
    if (cs === 0) return 0;
    return 1 - min(1, (1 - cb) / cs);
};

const blendLinear_f = (f) => (bottom, top) => {
    // rgba(false) keeps the floating-point [0, 255] channels intact
    const c0 = chroma(top).rgba(false);
    const c1 = chroma(bottom).rgba(false);
    if (c0[3] !== 1 || c1[3] !== 1) {
        throw new RangeError(
            "blend with space 'srgb-linear' only supports opaque colors (alpha === 1)"
        );
    }
    const out = [0, 1, 2].map((i) =>
        srgbEncode(f(srgbDecode(c0[i]), srgbDecode(c1[i])))
    );
    return chroma.rgb(out);
};

const srgbBlends = {
    normal: blend_f(each(normal)),
    multiply: blend_f(each(multiply)),
    screen: blend_f(each(screen)),
    overlay: blend_f(each(overlay)),
    darken: blend_f(each(darken)),
    lighten: blend_f(each(lighten)),
    dodge: blend_f(each(dodge)),
    burn: blend_f(each(burn))
};

const linearBlends = {
    normal: blendLinear_f(linearNormal),
    multiply: blendLinear_f(linearMultiply),
    screen: blendLinear_f(linearScreen),
    overlay: blendLinear_f(linearOverlay),
    darken: blendLinear_f(linearDarken),
    lighten: blendLinear_f(linearLighten),
    dodge: blendLinear_f(linearDodge),
    burn: blendLinear_f(linearBurn)
};

const resolveSpace = (options) => {
    const space =
        options && options.space !== undefined ? options.space : 'srgb';
    if (space !== 'srgb' && space !== 'srgb-linear') {
        throw new Error('unknown blend space ' + space);
    }
    return space;
};

Object.keys(srgbBlends).forEach((mode) => {
    blend[mode] = (bottom, top, options) => {
        const blends =
            resolveSpace(options) === 'srgb-linear' ? linearBlends : srgbBlends;
        return blends[mode](bottom, top);
    };
});

// blend.add = blend_f(each(add));

export default blend;
