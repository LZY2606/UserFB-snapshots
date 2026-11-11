/**
 * Regression contract for position-only stops ("hints"), e.g. `{ pos: 0.8 }`
 * without a `color`.
 *
 * Semantics under test (the library's existing convention, NOT the CSS
 * exponential color-hint curve):
 *  - the hint holds the 50% mid color of its two neighboring colored stops,
 *    placed at the hint's own position;
 *  - the gradient is then piecewise linear from the left stop to the hint and
 *    from the hint to the right stop;
 *  - the mid color is recomputed on every call in that call's own color space
 *    (encoded RGB + alpha for rgb*, HSV with the requested mode for hsv*),
 *    so a temporary color produced by a previous call can never leak into a
 *    later query;
 *  - hint stops stay colorless (`color: null`, `colorLess: true`) on the
 *    public `stops` and the constructor input array is never modified;
 *  - a hint must be strictly between its neighboring stops, while ordinary
 *    same-position colored stops (hard edges) remain allowed.
 */
import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import tinygradient from '../index.js';

/**
 * @returns {tinygradient.Instance} red at 0, hint at 0.8, blue at 1
 */
function hintedRedBlue() {
    return tinygradient([
        { color: 'red', pos: 0 },
        { pos: 0.8 },
        { color: 'blue', pos: 1 },
    ]);
}

/**
 * Assert every RGB channel is within `tol` bytes of the expected value.
 */
function assertCloseRgb(actual, expected, tol, label) {
    for (const channel of ['r', 'g', 'b']) {
        assert.ok(
            Math.abs(actual[channel] - expected[channel]) <= tol,
            `${label}: channel ${channel} is ${actual[channel]}, expected ~${expected[channel]} (±${tol})`
        );
    }
}

describe('position-only hint: direct queries on an unsampled gradient', () => {
    it('rgbAt() at the hint position does not throw and returns the 50% RGB midpoint', () => {
        const grad = hintedRedBlue();

        assert.deepStrictEqual(grad.rgbAt(0.8).toRgb(), { r: 128, g: 0, b: 128, a: 1 });
    });

    it('rgbAt() between the stops does not throw and follows the left segment', () => {
        const grad = hintedRedBlue();

        // segment parameter is 0.4 / 0.8 = 0.5 between red and the purple midpoint
        assert.deepStrictEqual(grad.rgbAt(0.4).toRgb(), { r: 192, g: 0, b: 64, a: 1 });
    });

    it('hsvAt() at the hint position does not throw and keeps the ordinary hue convention', () => {
        const grad = hintedRedBlue();

        // clockwise red -> blue midpoint is hue 120 (green), same as red->blue without a hint
        assert.deepStrictEqual(grad.hsvAt(0.8).toHsv(), { h: 120, s: 1, v: 1, a: 1 });
    });
});

describe('position-only hint: interleaved sampling stays deterministic', () => {
    it('rgbAt() gives the same answer whatever rgb/hsv batch calls ran before', () => {
        const direct = hintedRedBlue().rgbAt(0.8).toRgb();

        const afterRgb = hintedRedBlue();
        afterRgb.rgb(5);

        const afterHsvShort = hintedRedBlue();
        afterHsvShort.hsv(5, 'short');

        const afterMixed = hintedRedBlue();
        afterMixed.rgb(7);
        afterMixed.hsv(9, 'long');
        afterMixed.rgb(3);
        afterMixed.hsv(11, false);

        assert.deepStrictEqual(afterRgb.rgbAt(0.8).toRgb(), direct, 'result changed after rgb(5)');
        assert.deepStrictEqual(afterHsvShort.rgbAt(0.8).toRgb(), direct, 'result changed after hsv(5, "short")');
        assert.deepStrictEqual(afterMixed.rgbAt(0.8).toRgb(), direct, 'result changed after mixed batch calls');
    });

    it('repeating the same query always returns the same color', () => {
        const grad = hintedRedBlue();
        grad.rgb(5);
        grad.hsv(5, 'short');

        for (const pos of [0, 0.1, 0.4, 0.8, 0.9, 1]) {
            assert.deepStrictEqual(grad.rgbAt(pos).toRgb(), grad.rgbAt(pos).toRgb(), `rgbAt(${pos}) is not repeatable`);
            assert.deepStrictEqual(grad.hsvAt(pos).toHsv(), grad.hsvAt(pos).toHsv(), `hsvAt(${pos}) is not repeatable`);
        }
    });

    it('hsvAt() is unaffected by the mode used by previous hsv() batch calls', () => {
        const direct = hintedRedBlue().hsvAt(0.8).toHsv();

        const afterShort = hintedRedBlue();
        afterShort.hsv(5, 'short');

        const afterLong = hintedRedBlue();
        afterLong.hsv(7, 'long');
        afterLong.rgb(4);

        assert.deepStrictEqual(afterShort.hsvAt(0.8).toHsv(), direct, 'hsvAt leaked the "short" mode');
        assert.deepStrictEqual(afterLong.hsvAt(0.8).toHsv(), direct, 'hsvAt leaked the "long" mode');
    });

    it('hsv() batch entries still honor the requested mode direction', () => {
        const grad = hintedRedBlue();

        const clockwise = grad.hsv(11, false).map(c => c.toHex());
        const longest = grad.hsv(11, 'long').map(c => c.toHex());

        assert.notDeepStrictEqual(clockwise, longest, 'HSV "long" mode no longer changes the interpolation direction');
    });

    it('rgb() and hsv() keep the requested number of samples', () => {
        const grad = hintedRedBlue();

        assert.strictEqual(grad.rgb(13).length, 13, 'rgb() sample count changed');
        assert.strictEqual(grad.hsv(17, 'short').length, 17, 'hsv() sample count changed');
    });
});

describe('position-only hint: piecewise RGB numbers', () => {
    it('black -> hint at 0.8 -> white yields ~64 / 128 / 191 at 0.4 / 0.8 / 0.9', () => {
        const grad = tinygradient([
            { color: 'black', pos: 0 },
            { pos: 0.8 },
            { color: 'white', pos: 1 },
        ]);

        const expected = {
            0.4: 64,
            0.8: 128,
            0.9: 191, // exact interpolation gives 192, TinyColor rounding tolerance covers the 1-byte gap
        };

        for (const [pos, value] of Object.entries(expected)) {
            const rgb = grad.rgbAt(Number(pos)).toRgb();
            assertCloseRgb(rgb, { r: value, g: value, b: value }, 1, `rgbAt(${pos})`);
        }
    });
});

describe('position-only hint: alpha follows the same two segments', () => {
    it('interpolates alpha through the 50% midpoint in RGB and HSV', () => {
        const grad = tinygradient([
            { color: { r: 255, g: 0, b: 0, a: 0.25 }, pos: 0 },
            { pos: 0.8 },
            { color: { r: 0, g: 0, b: 255, a: 0.75 }, pos: 1 },
        ]);

        const at04 = grad.rgbAt(0.4).toRgb();
        const at08 = grad.rgbAt(0.8).toRgb();
        const at09 = grad.rgbAt(0.9).toRgb();

        assert.deepStrictEqual(
            { r: at04.r, g: at04.g, b: at04.b },
            { r: 192, g: 0, b: 64 },
            'rgbAt(0.4) channels do not match the left segment'
        );
        assert.deepStrictEqual(
            { r: at08.r, g: at08.g, b: at08.b },
            { r: 128, g: 0, b: 128 },
            'rgbAt(0.8) channels do not match the midpoint'
        );
        assert.deepStrictEqual(
            { r: at09.r, g: at09.g, b: at09.b },
            { r: 64, g: 0, b: 192 },
            'rgbAt(0.9) channels do not match the right segment'
        );

        assert.ok(Math.abs(at04.a - 0.375) < 1e-9, `alpha at 0.4 is ${at04.a}, expected 0.375`);
        assert.ok(Math.abs(at08.a - 0.5) < 1e-9, `alpha at 0.8 is ${at08.a}, expected 0.5`);
        assert.ok(Math.abs(at09.a - 0.625) < 1e-9, `alpha at 0.9 is ${at09.a}, expected 0.625`);

        assert.ok(Math.abs(grad.hsvAt(0.8).toRgb().a - 0.5) < 1e-9, 'HSV midpoint alpha is not 0.5');
    });
});

describe('position-only hint: reverse()', () => {
    it('returns an independent gradient and keeps the hint colorless at 1-pos', () => {
        const grad = hintedRedBlue();
        const reversed = grad.reverse();

        assert.notEqual(reversed, grad, 'reverse() must return a new instance');
        assert.notEqual(reversed.stops, grad.stops, 'reverse() must not reuse the stops array');

        const reversedHint = reversed.stops.find(s => s.colorLess === true);
        assert.ok(reversedHint, 'the reversed gradient lost its colorless hint stop');
        assert.strictEqual(reversedHint.color, null, 'the reversed hint gained a color');
        assert.ok(Math.abs(reversedHint.pos - (1 - 0.8)) < 1e-12, `reversed hint pos is ${reversedHint.pos}, expected 0.2`);

        // sampling one gradient must not touch the other one
        grad.rgb(9);
        grad.hsv(9, 'long');
        assert.strictEqual(reversedHint.color, null, 'sampling the original mutated the reversed hint');
        assert.strictEqual(reversedHint.colorLess, true, 'sampling the original removed colorLess on the reversed hint');
    });

    it('samples complementarily to the original gradient', () => {
        const grad = hintedRedBlue();
        const reversed = grad.reverse();

        for (const pos of [0, 0.1, 0.4, 0.8, 0.9, 1]) {
            const forward = grad.rgbAt(pos).toRgb();
            const backward = reversed.rgbAt(1 - pos).toRgb();
            assertCloseRgb(backward, forward, 1, `reverse mismatch at pos ${pos}`);
            assert.ok(Math.abs(backward.a - forward.a) < 1e-9, `reverse alpha mismatch at pos ${pos}`);
        }
    });
});

describe('position-only hint: strict position boundaries', () => {
    it('throws when the hint shares a position with a neighboring stop', () => {
        const invalid = [
            // hint on the left neighbor
            [{ color: 'black', pos: 0 }, { pos: 0 }, { color: 'white', pos: 1 }],
            // hint on the right neighbor
            [{ color: 'black', pos: 0 }, { pos: 1 }, { color: 'white', pos: 1 }],
            // hint on a middle colored stop, left side
            [{ color: 'black', pos: 0 }, { color: 'red', pos: 0.5 }, { pos: 0.5 }, { color: 'white', pos: 1 }],
            // hint on a middle colored stop, right side
            [{ color: 'black', pos: 0 }, { pos: 0.5 }, { color: 'red', pos: 0.5 }, { color: 'white', pos: 1 }],
        ];

        for (const stops of invalid) {
            assert.throws(() => tinygradient(stops), Error, `should reject hint at pos ${stops[1].pos}`);
        }
    });

    it('still allows same-position colored stops and keeps their hard edge', () => {
        const grad = tinygradient([
            { color: 'black', pos: 0 },
            { color: 'white', pos: 0.5 },
            { color: 'black', pos: 0.5 },
            { color: 'white', pos: 1 },
        ]);

        assert.deepStrictEqual(
            grad.rgb(8).map(c => c.toHex()),
            ['000000', '555555', 'aaaaaa', 'ffffff', '000000', '555555', 'aaaaaa', 'ffffff']
        );
    });
});

describe('position-only hint: representation and input immutability', () => {
    it('keeps color:null / colorLess:true on the public stops after any sampling', () => {
        const grad = hintedRedBlue();
        grad.rgb(5);
        grad.hsv(5, 'short');
        grad.rgbAt(0.8);
        grad.hsvAt(0.8);
        grad.reverse();

        const hint = grad.stops[1];
        assert.strictEqual(hint.color, null, 'the hint stop gained a persisted color');
        assert.strictEqual(hint.colorLess, true, 'the hint stop lost its colorLess flag');
        assert.strictEqual(hint.pos, 0.8, 'the hint position changed');
    });

    it('never modifies the constructor input array', () => {
        const input = [
            { color: 'red', pos: 0 },
            { pos: 0.8 },
            { color: 'blue', pos: 1 },
        ];
        const snapshot = JSON.stringify(input);

        const grad = tinygradient(input);
        grad.rgb(5);
        grad.hsv(5, 'short');
        grad.rgbAt(0.8);
        grad.hsvAt(0.8);
        grad.reverse();

        assert.strictEqual(JSON.stringify(input), snapshot, 'the input array was mutated');
        assert.strictEqual('color' in input[1], false, 'a color key was added to the input hint');
    });

    it('still renders the hint without a color in css()', () => {
        const grad = hintedRedBlue();

        assert.strictEqual(
            grad.css(),
            'linear-gradient(to right, rgb(255, 0, 0) 0%, 80%, rgb(0, 0, 255) 100%)'
        );
    });
});

describe('ordinary gradients keep their existing behavior', () => {
    it('rgbAt/hsvAt on plain two-stop gradients are unchanged', () => {
        assert.deepStrictEqual(
            tinygradient('white', 'black').rgbAt(0.5).toRgb(),
            { r: 128, g: 128, b: 128, a: 1 }
        );
        assert.deepStrictEqual(
            tinygradient('red', 'blue').hsvAt(0.5).toHsv(),
            { h: 120, s: 1, v: 1, a: 1 }
        );
    });

    it('position-only batch rgb() output is unchanged', () => {
        const grad = tinygradient([
            { color: 'black', pos: 0 },
            { pos: 0.2 },
            { color: 'white', pos: 1 },
        ]);

        assert.deepStrictEqual(
            grad.rgb(5).map(c => c.toHex()),
            ['000000', '808080', 'aaaaaa', 'd5d5d5', 'ffffff']
        );
    });
});
