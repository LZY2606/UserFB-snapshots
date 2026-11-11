import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import chroma, { blend } from '../index.js';

const EPS = 1e-6;

// Independent reference implementation of the sRGB transfer function and the
// eight blend modes, written only from the specification (library internals
// are not reused here).
const { min, max, pow, round } = Math;

const srgbDecode = (c) => {
    c = c / 255;
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
};

const srgbEncode = (l) =>
    (l <= 0.0031308 ? 12.92 * l : 1.055 * pow(l, 1 / 2.4) - 0.055) * 255;

const LINEAR_MODES = {
    // cs = source (top), cb = backdrop (bottom)
    normal: (cs) => cs,
    multiply: (cs, cb) => cs * cb,
    screen: (cs, cb) => 1 - (1 - cb) * (1 - cs),
    darken: (cs, cb) => min(cs, cb),
    lighten: (cs, cb) => max(cs, cb),
    overlay: (cs, cb) =>
        cb <= 0.5 ? 2 * cb * cs : 1 - 2 * (1 - cb) * (1 - cs),
    dodge: (cs, cb) => {
        if (cb === 0) return 0;
        if (cs === 1) return 1;
        return min(1, cb / (1 - cs));
    },
    burn: (cs, cb) => {
        if (cb === 1) return 1;
        if (cs === 0) return 0;
        return 1 - min(1, (1 - cb) / cs);
    }
};

const reference = (bottom, top, mode) => {
    const fn = LINEAR_MODES[mode];
    return bottom.map((_, i) =>
        srgbEncode(fn(srgbDecode(top[i]), srgbDecode(bottom[i])))
    );
};

const closeArray = (actual, expected, eps = EPS) => {
    assert.equal(actual.length, expected.length);
    actual.forEach((v, i) =>
        assert.ok(
            Math.abs(v - expected[i]) <= eps,
            `channel ${i}: ${v} not within ${eps} of ${expected[i]}`
        )
    );
};

const LINEAR = { space: 'srgb-linear' };
const SRGB = { space: 'srgb' };

const BOTTOM = [200, 128, 30];
const TOP = [60, 170, 240];
const MODES = Object.keys(LINEAR_MODES);

describe('linear-light (srgb-linear) blending', () => {
    it('exposes the same blend function from the root export and chroma.blend', () => {
        assert.equal(chroma.blend, blend);
    });

    MODES.forEach((mode) => {
        it(`computes ${mode} against an independent formula on asymmetric inputs`, () => {
            const result = blend(BOTTOM, TOP, mode, LINEAR);
            closeArray(result.rgb(false), reference(BOTTOM, TOP, mode));
            assert.equal(result.alpha(), 1);
        });
    });

    it('matches the two numeric values that separate the new and old paths', () => {
        const screen = blend(
            [128, 128, 128],
            [128, 128, 128],
            'screen',
            LINEAR
        );
        closeArray(
            screen.rgb(false),
            [166.7451845662336, 166.7451845662336, 166.7451845662336]
        );
        assert.notDeepEqual(
            screen.rgb(false),
            blend([128, 128, 128], [128, 128, 128], 'screen').rgb(false)
        );

        const overlay = blend([128, 128, 128], 'white', 'overlay', LINEAR);
        closeArray(
            overlay.rgb(false),
            [175.55563028849915, 175.55563028849915, 175.55563028849915]
        );
        assert.deepEqual(
            blend([128, 128, 128], 'white', 'overlay').rgb(),
            [255, 255, 255]
        );
    });

    it('keeps floating-point channels in rgb(false) but rounds rgb() and hex()', () => {
        const result = blend(
            [128, 128, 128],
            [128, 128, 128],
            'screen',
            LINEAR
        );
        assert.ok(result.rgb(false).every((v) => !Number.isInteger(v)));
        assert.deepEqual(result.rgb(), [167, 167, 167]);
        assert.equal(result.hex(), '#a7a7a7');
    });

    it('accepts options through all eight shortcut methods', () => {
        MODES.forEach((mode) => {
            closeArray(
                blend[mode](BOTTOM, TOP, LINEAR).rgb(false),
                blend(BOTTOM, TOP, mode, LINEAR).rgb(false)
            );
        });
    });

    it('decodes both sides of the sRGB linear-segment threshold', () => {
        // 10.31 decodes just below 0.0031308, 10.32 just above it; normal mode
        // therefore round-trips each value through a different branch.
        closeArray(
            blend(
                [10.31, 10.31, 10.31],
                [10.31, 10.31, 10.31],
                'normal',
                LINEAR
            ).rgb(false),
            [10.31, 10.31, 10.31]
        );
        closeArray(
            blend(
                [10.32, 10.32, 10.32],
                [10.32, 10.32, 10.32],
                'normal',
                LINEAR
            ).rgb(false),
            [10.32, 10.32, 10.32]
        );
        // the two decoded values really do straddle the encode branch boundary
        assert.ok(srgbDecode(10.31) <= 0.0031308);
        assert.ok(srgbDecode(10.32) > 0.0031308);
        // and the encoded outputs land on opposite sides of the decode boundary
        assert.ok(srgbEncode(srgbDecode(10.31)) <= 0.04045 * 255);
        assert.ok(srgbEncode(srgbDecode(10.32)) > 0.04045 * 255);
    });

    it('honors the linear-domain edge branches of dodge and burn', () => {
        // dodge: black backdrop yields black even with a bright source
        closeArray(
            blend([0, 0, 0], [100, 100, 100], 'dodge', LINEAR).rgb(false),
            [0, 0, 0]
        );
        // dodge: a white source (linear 1) yields white
        closeArray(
            blend([10, 10, 10], [255, 255, 255], 'dodge', LINEAR)
                .rgb(false)
                .map((v) => v / 255),
            [1, 1, 1]
        );
        // burn: white backdrop yields white even with a dark source
        closeArray(
            blend([255, 255, 255], [100, 100, 100], 'burn', LINEAR)
                .rgb(false)
                .map((v) => v / 255),
            [1, 1, 1]
        );
        // burn: black source (linear 0) yields black
        closeArray(
            blend([10, 10, 10], [0, 0, 0], 'burn', LINEAR).rgb(false),
            [0, 0, 0]
        );
    });

    it('decides overlay branches on the linear 0.5 boundary, not encoded 128', () => {
        // encoded 128 is well below linear 0.5 (~0.216), so the dark branch is
        // used even though the encoded value exceeds 127.5
        const gray = [128, 128, 128];
        closeArray(
            blend(gray, gray, 'overlay', LINEAR).rgb(false),
            reference(gray, gray, 'overlay')
        );
        // an encoded channel whose linear value sits just above 0.5 takes the
        // light branch: encoded 188 decodes to ~0.502
        const nearHalf = [188, 188, 188];
        assert.ok(srgbDecode(188) > 0.5);
        closeArray(
            blend(nearHalf, TOP, 'overlay', LINEAR).rgb(false),
            reference(nearHalf, TOP, 'overlay')
        );
    });

    it('blends three floating-point colors with screen without intermediate byte rounding', () => {
        const c1 = [10.31, 10.32, 200.25];
        const c2 = [50.5, 120.75, 99.9];
        const c3 = [170.125, 30.3, 8.8];

        const screenLinear = (a, b) =>
            a.map((_, i) => 1 - (1 - a[i]) * (1 - b[i]));
        const toLinear = (c) => c.map(srgbDecode);
        const floatRefA = screenLinear(
            screenLinear(toLinear(c1), toLinear(c2)),
            toLinear(c3)
        ).map(srgbEncode);
        const floatRefB = screenLinear(
            toLinear(c1),
            screenLinear(toLinear(c2), toLinear(c3))
        ).map(srgbEncode);

        const orderA = blend(
            blend(c1, c2, 'screen', LINEAR),
            c3,
            'screen',
            LINEAR
        );
        const orderB = blend(
            c1,
            blend(c2, c3, 'screen', LINEAR),
            'screen',
            LINEAR
        );

        closeArray(orderA.rgb(false), floatRefA, 1e-10);
        closeArray(orderB.rgb(false), floatRefB, 1e-10);
        // screen is associative in the linear domain
        closeArray(orderA.rgb(false), orderB.rgb(false), 1e-10);

        // a pipeline that rounds to bytes after every step cannot reproduce this
        const integerizedStep = (a, b) =>
            screenLinear(
                a.map((v) => srgbDecode(round(v))),
                b.map((v) => srgbDecode(round(v)))
            ).map((l) => round(srgbEncode(l)));
        const integerized = integerizedStep(integerizedStep(c1, c2), c3);
        assert.ok(
            Math.abs(orderA.rgb(false)[1] - integerized[1]) > 0.4,
            `expected a visible gap from byte rounding, got ${orderA.rgb(false)[1]} vs ${integerized[1]}`
        );
    });

    it('does not swap the roles of bottom and top', () => {
        ['normal', 'overlay', 'dodge'].forEach((mode) => {
            const forward = blend(BOTTOM, TOP, mode, LINEAR).rgb(false);
            const swapped = blend(TOP, BOTTOM, mode, LINEAR).rgb(false);
            assert.ok(
                forward.some((v, i) => Math.abs(v - swapped[i]) > 1e-6),
                `${mode} produced identical results after swapping arguments`
            );
            closeArray(forward, reference(BOTTOM, TOP, mode));
            closeArray(swapped, reference(TOP, BOTTOM, mode));
        });
    });
});

describe('linear-mode restrictions and error handling', () => {
    it('throws RangeError for translucent inputs in either position', () => {
        assert.throws(
            () =>
                blend([128, 128, 128, 0.5], [128, 128, 128], 'normal', LINEAR),
            (err) => err instanceof RangeError
        );
        assert.throws(
            () =>
                blend([128, 128, 128], [128, 128, 128, 0.5], 'normal', LINEAR),
            (err) => err instanceof RangeError
        );
        assert.throws(
            () =>
                blend(
                    chroma.rgb(128, 128, 128),
                    chroma.rgb(128, 128, 128).alpha(0.999999),
                    'screen',
                    LINEAR
                ),
            (err) => err instanceof RangeError
        );
        // shortcuts enforce the same restriction
        assert.throws(
            () => blend.screen([128, 128, 128, 0.5], [128, 128, 128], LINEAR),
            (err) => err instanceof RangeError
        );
        // exactly opaque four-channel input is accepted
        assert.doesNotThrow(() =>
            blend([1, 2, 3, 1], [4, 5, 6, 1], 'normal', LINEAR)
        );
    });

    it('throws a plain Error for an unknown space', () => {
        assert.throws(
            () => blend([1, 2, 3], [4, 5, 6], 'normal', { space: 'lab' }),
            (err) =>
                err instanceof Error &&
                err.name === 'Error' &&
                /unknown blend space/.test(err.message)
        );
        assert.throws(
            () => blend.screen([1, 2, 3], [4, 5, 6], { space: 'lrgb' }),
            (err) => err instanceof Error
        );
    });

    it('still rejects unknown blend modes in either space', () => {
        assert.throws(
            () => blend([1, 2, 3], [4, 5, 6], 'nope', LINEAR),
            /unknown blend mode/
        );
        assert.throws(
            () => blend([1, 2, 3], [4, 5, 6], 'nope'),
            /unknown blend mode/
        );
    });

    it('leaves the input colors untouched after a linear call or a thrown error', () => {
        const bottom = chroma.rgb(10.31, 20.5, 30.7);
        const top = chroma.rgb(40.2, 50.1, 60.9);
        const translucentTop = chroma.rgb(40.2, 50.1, 60.9).alpha(0.5);
        const bottomSnapshot = bottom.rgba(false);
        const topSnapshot = top.rgba(false);
        blend(bottom, top, 'screen', LINEAR);
        assert.deepEqual(bottom.rgba(false), bottomSnapshot);
        assert.deepEqual(top.rgba(false), topSnapshot);

        assert.throws(() => blend(bottom, translucentTop, 'screen', LINEAR));
        assert.deepEqual(bottom.rgba(false), bottomSnapshot);
        assert.deepEqual(translucentTop.rgba(false), [40.2, 50.1, 60.9, 0.5]);
        assert.throws(() => blend(bottom, top, 'screen', { space: 'bogus' }));
        assert.deepEqual(bottom.rgba(false), bottomSnapshot);
        assert.deepEqual(top.rgba(false), topSnapshot);
    });

    it('keeps the default encoded-srgb path working after linear calls and errors', () => {
        // exercise the new path and both error paths first
        blend(BOTTOM, TOP, 'overlay', LINEAR);
        assert.throws(() => blend(BOTTOM, [1, 2, 3, 0.5], 'normal', LINEAR));
        assert.throws(() => blend(BOTTOM, TOP, 'normal', { space: 'nope' }));

        // the default path still computes in the encoded domain, incl. the
        // value the linear path deliberately does not produce
        closeArray(
            blend([128, 128, 128], [128, 128, 128], 'screen').rgb(false),
            [191.74901960784314, 191.74901960784314, 191.74901960784314]
        );
        assert.deepEqual(
            blend([128, 128, 128], 'white', 'overlay').rgb(),
            [255, 255, 255]
        );

        // an empty options object and an explicit srgb space use the old path too
        assert.deepEqual(
            blend(BOTTOM, TOP, 'screen', {}).rgba(false),
            blend(BOTTOM, TOP, 'screen').rgba(false)
        );
        assert.deepEqual(
            blend(BOTTOM, TOP, 'screen', SRGB).rgba(false),
            blend(BOTTOM, TOP, 'screen').rgba(false)
        );
        assert.deepEqual(
            blend.screen(BOTTOM, TOP).rgba(false),
            blend(BOTTOM, TOP, 'screen').rgba(false)
        );
        assert.deepEqual(
            blend.screen(BOTTOM, TOP, SRGB).rgba(false),
            blend(BOTTOM, TOP, 'screen').rgba(false)
        );
    });
});

describe('encoded-srgb path compatibility (including translucency)', () => {
    it('preserves the existing alpha compositing behavior', () => {
        const result = blend(
            chroma.rgb(160, 160, 160).alpha(0.5),
            chroma.rgb(80, 80, 80).alpha(0.5),
            'screen'
        );
        closeArray(
            result.rgb(false),
            [143.26797385620915, 143.26797385620915, 143.26797385620915]
        );
        assert.equal(result.alpha(), 0.75);

        assert.deepEqual(
            blend([0, 0, 255, 0.25], [255, 0, 0, 0.5], 'normal').rgba(false),
            [204, 0, 51, 0.625]
        );
        assert.deepEqual(
            blend([10, 20, 30, 0], [40, 50, 60, 0], 'normal').rgba(),
            [0, 0, 0, 0]
        );
        assert.equal(blend('blue', '#ff000000', 'normal').hex(), '#0000ff');
    });

    it('keeps the old opaque results unchanged for every mode and shortcut', () => {
        MODES.forEach((mode) => {
            assert.deepEqual(
                blend[mode](BOTTOM, TOP).rgba(false),
                blend(BOTTOM, TOP, mode).rgba(false)
            );
        });
        assert.equal(blend('red', '#5a9f37', 'multiply').hex(), '#5a0000');
    });

    it('continues to handle translucent colors on the srgb path after a linear call', () => {
        blend(BOTTOM, TOP, 'burn', LINEAR);
        const translucent = blend([80, 80, 80], [50, 50, 50, 0.5], 'burn');
        assert.deepEqual(translucent.rgba(false), [40, 40, 40, 1]);
    });
});
