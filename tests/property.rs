//! Property tests for CSS Color 4 parsing, serialization and conversion.
//!
//! Colors are generated as *syntax trees* (never random strings) and the
//! expected values are computed by an independent `f64` reference model
//! (`mod reference`) that encodes, per component: its domain, how angles
//! wrap, how alpha is clamped and which components may be `none` (missing).
//!
//! Properties under test:
//!
//! 1. Round-trip ([roundtrip_parse_serialize_parse]): for the representable
//!    subset of inputs, parse -> serialize -> parse yields the same value
//!    (and the same string).
//! 2. Conversion ([conversions_match_f64_reference]): cross-gamut
//!    conversions match the high-precision reference, with each color
//!    space's own error definition (no single global epsilon).
//! 3. Evaluation ([absolute_parse_matches_reference],
//!    [relative_parse_matches_reference]): generated absolute and relative
//!    color syntax trees evaluate to the independently computed value.

use csscolorparser::{Color, parse};
use std::f64::consts::{PI, TAU};

// ---------------------------------------------------------------------
// Deterministic RNG (xorshift64*). No external dependencies.
// ---------------------------------------------------------------------

struct Rng(u64);

impl Rng {
    fn new(seed: u64) -> Self {
        Self(seed | 1)
    }

    fn next(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        self.0 = x;
        x.wrapping_mul(0x2545_F491_4F6C_DD1D)
    }

    fn below(&mut self, n: usize) -> usize {
        (self.next() % n as u64) as usize
    }

    fn f64(&mut self) -> f64 {
        (self.next() >> 11) as f64 / 9007199254740992.0
    }

    fn range(&mut self, lo: f64, hi: f64) -> f64 {
        lo + (hi - lo) * self.f64()
    }

    fn chance(&mut self, p: f64) -> bool {
        self.f64() < p
    }
}

// ---------------------------------------------------------------------
// Independent f64 reference model.
//
// Mirrors the library's conversion formulas and `Color::from_*` domain
// rules (clamps, angle normalization) in f64 precision.
// ---------------------------------------------------------------------

mod reference {
    /// `f32::EPSILON` as f64: the library's f32 branch thresholds, mirrored.
    const EPS: f64 = 1.192_092_9e-7;

    pub fn normalize_angle(t: f64) -> f64 {
        ((t % 360.0) + 360.0) % 360.0
    }

    fn hue_to_rgb(n1: f64, n2: f64, h: f64) -> f64 {
        let h = (h % 6.0 + 6.0) % 6.0;
        if h < 1.0 {
            n1 + (n2 - n1) * h
        } else if h < 3.0 {
            n2
        } else if h < 4.0 {
            n1 + (n2 - n1) * (4.0 - h)
        } else {
            n1
        }
    }

    /// h: [0..360], s, l: [0..1]
    pub fn hsl_to_rgb(h: f64, s: f64, l: f64) -> [f64; 3] {
        if s.abs() < EPS {
            return [l, l, l];
        }
        let n2 = if l < 0.5 {
            l * (1.0 + s)
        } else {
            l + s - l * s
        };
        let n1 = 2.0 * l - n2;
        let h = h / 60.0;
        [
            hue_to_rgb(n1, n2, h + 2.0),
            hue_to_rgb(n1, n2, h),
            hue_to_rgb(n1, n2, h - 2.0),
        ]
    }

    pub fn hwb_to_rgb(hue: f64, white: f64, black: f64) -> [f64; 3] {
        if white + black >= 1.0 {
            let l = white / (white + black);
            return [l, l, l];
        }
        let [r, g, b] = hsl_to_rgb(hue, 1.0, 0.5);
        let f = |x: f64| x * (1.0 - white - black) + white;
        [f(r), f(g), f(b)]
    }

    pub fn hsv_to_rgb(h: f64, s: f64, v: f64) -> [f64; 3] {
        // Via HSL, mirroring the library.
        let l = (2.0 - s) * v / 2.0;
        let s = if l.abs() > EPS {
            if (l - 1.0).abs() < EPS {
                0.0
            } else if l < 0.5 {
                s * v / (l * 2.0)
            } else {
                s * v / (2.0 - l * 2.0)
            }
        } else {
            s
        };
        hsl_to_rgb(h, s, l)
    }

    pub fn rgb_to_hsv(r: f64, g: f64, b: f64) -> [f64; 3] {
        let v = r.max(g.max(b));
        let d = v - r.min(g.min(b));
        if d.abs() < EPS {
            return [0.0, 0.0, v];
        }
        let s = d / v;
        let dr = (v - r) / d;
        let dg = (v - g) / d;
        let db = (v - b) / d;
        let h = if (r - v).abs() < EPS {
            db - dg
        } else if (g - v).abs() < EPS {
            2.0 + dr - db
        } else {
            4.0 + dg - dr
        };
        [normalize_angle((h * 60.0) % 360.0), s, v]
    }

    pub fn rgb_to_hsl(r: f64, g: f64, b: f64) -> [f64; 3] {
        let min = r.min(g.min(b));
        let max = r.max(g.max(b));
        let l = (max + min) / 2.0;
        if (min - max).abs() < EPS {
            return [0.0, 0.0, l];
        }
        let d = max - min;
        let s = if l < 0.5 {
            d / (max + min)
        } else {
            d / (2.0 - max - min)
        };
        let dr = (max - r) / d;
        let dg = (max - g) / d;
        let db = (max - b) / d;
        let h = if (r - max).abs() < EPS {
            db - dg
        } else if (g - max).abs() < EPS {
            2.0 + dr - db
        } else {
            4.0 + dg - dr
        };
        [normalize_angle((h * 60.0) % 360.0), s, l]
    }

    pub fn rgb_to_hwb(r: f64, g: f64, b: f64) -> [f64; 3] {
        let [h, _, _] = rgb_to_hsl(r, g, b);
        [h, r.min(g.min(b)), 1.0 - r.max(g.max(b))]
    }

    pub fn to_linear(x: f64) -> f64 {
        if x >= 0.04045 {
            ((x + 0.055) / 1.055).powf(2.4)
        } else {
            x / 12.92
        }
    }

    pub fn from_linear(x: f64) -> f64 {
        if x >= 0.0031308 {
            1.055 * x.powf(1.0 / 2.4) - 0.055
        } else {
            12.92 * x
        }
    }

    pub fn linear_rgb_to_xyz_d65(r: f64, g: f64, b: f64) -> [f64; 3] {
        [
            0.4124564 * r + 0.3575761 * g + 0.1804375 * b,
            0.2126729 * r + 0.7151522 * g + 0.0721750 * b,
            0.0193339 * r + 0.119192 * g + 0.9503041 * b,
        ]
    }

    pub fn xyz_d65_to_linear_rgb(x: f64, y: f64, z: f64) -> [f64; 3] {
        [
            3.2404542 * x - 1.5371385 * y - 0.4985314 * z,
            -0.969266 * x + 1.8760108 * y + 0.0415560 * z,
            0.0556434 * x - 0.2040259 * y + 1.0572252 * z,
        ]
    }

    pub fn linear_rgb_to_xyz_d50(r: f64, g: f64, b: f64) -> [f64; 3] {
        [
            0.4360747 * r + 0.3850649 * g + 0.1430804 * b,
            0.2225045 * r + 0.7168786 * g + 0.0606169 * b,
            0.0139322 * r + 0.0971045 * g + 0.7141733 * b,
        ]
    }

    pub fn xyz_d50_to_linear_rgb(x: f64, y: f64, z: f64) -> [f64; 3] {
        [
            3.1338561 * x - 1.6168667 * y - 0.4906146 * z,
            -0.9787684 * x + 1.9161415 * y + 0.0334540 * z,
            0.0719453 * x - 0.2289914 * y + 1.4052427 * z,
        ]
    }

    /// LMS cone responses (pre-cbrt) of the Oklab transform. The inverse
    /// transform is ill-conditioned where any of these crosses zero:
    /// `cbrt` has unbounded derivative at 0 and amplifies f32 rounding.
    pub fn linear_rgb_to_lms(r: f64, g: f64, b: f64) -> [f64; 3] {
        [
            0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b,
            0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b,
            0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b,
        ]
    }

    pub fn linear_rgb_to_oklab(r: f64, g: f64, b: f64) -> [f64; 3] {
        let l_ = (0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b).cbrt();
        let m_ = (0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b).cbrt();
        let s_ = (0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b).cbrt();
        [
            0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,
            1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
            0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_,
        ]
    }

    pub fn oklab_to_linear_rgb(l: f64, a: f64, b: f64) -> [f64; 3] {
        let l_ = (l + 0.3963377774 * a + 0.2158037573 * b).powi(3);
        let m_ = (l - 0.1055613458 * a - 0.0638541728 * b).powi(3);
        let s_ = (l - 0.0894841775 * a - 1.2914855480 * b).powi(3);
        [
            4.0767416621 * l_ - 3.3077115913 * m_ + 0.2309699292 * s_,
            -1.2684380046 * l_ + 2.6097574011 * m_ - 0.3413193965 * s_,
            -0.0041960863 * l_ - 0.7034186147 * m_ + 1.7076147010 * s_,
        ]
    }

    // D50 illuminant (normalized to Y = 1.0), same constants as the library.
    const D50_X: f64 = 0.96422;
    const D50_Y: f64 = 1.0;
    const D50_Z: f64 = 0.82521;
    const DELTA: f64 = 6.0 / 29.0;

    pub fn linear_rgb_to_lab(r: f64, g: f64, b: f64) -> [f64; 3] {
        let [x, y, z] = linear_rgb_to_xyz_d50(r, g, b);
        let f = |t: f64| {
            if t > DELTA * DELTA * DELTA {
                t.cbrt()
            } else {
                t / (3.0 * DELTA * DELTA) + 4.0 / 29.0
            }
        };
        let fx = f(x / D50_X);
        let fy = f(y / D50_Y);
        let fz = f(z / D50_Z);
        [116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)]
    }

    pub fn lab_to_linear_rgb(l: f64, a: f64, b: f64) -> [f64; 3] {
        let fy = (l + 16.0) / 116.0;
        let fx = fy + a / 500.0;
        let fz = fy - b / 200.0;
        let f = |t: f64| {
            if t > DELTA {
                t * t * t
            } else {
                (t - 16.0 / 116.0) * 3.0 * DELTA * DELTA
            }
        };
        xyz_d50_to_linear_rgb(D50_X * f(fx), D50_Y * f(fy), D50_Z * f(fz))
    }

    // -- `Color::from_*` equivalents: domain rules (clamps, angle wrap) --

    pub fn from_hsla(h: f64, s: f64, l: f64, a: f64) -> [f64; 4] {
        let [r, g, b] = hsl_to_rgb(normalize_angle(h), s.clamp(0.0, 1.0), l.clamp(0.0, 1.0));
        [r, g, b, a]
    }

    pub fn from_hsva(h: f64, s: f64, v: f64, a: f64) -> [f64; 4] {
        let [r, g, b] = hsv_to_rgb(normalize_angle(h), s.clamp(0.0, 1.0), v.clamp(0.0, 1.0));
        [r, g, b, a]
    }

    pub fn from_hwba(h: f64, w: f64, b: f64, a: f64) -> [f64; 4] {
        let [r, g, b] = hwb_to_rgb(normalize_angle(h), w.clamp(0.0, 1.0), b.clamp(0.0, 1.0));
        [r, g, b, a]
    }

    pub fn from_linear_rgba(r: f64, g: f64, b: f64, a: f64) -> [f64; 4] {
        [from_linear(r), from_linear(g), from_linear(b), a]
    }

    pub fn from_oklaba(l: f64, a: f64, b: f64, alpha: f64) -> [f64; 4] {
        let [r, g, b] = oklab_to_linear_rgb(l, a, b);
        from_linear_rgba(r, g, b, alpha)
    }

    pub fn from_oklcha(l: f64, c: f64, h: f64, alpha: f64) -> [f64; 4] {
        from_oklaba(l, c * h.cos(), c * h.sin(), alpha)
    }

    pub fn from_laba(l: f64, a: f64, b: f64, alpha: f64) -> [f64; 4] {
        let [r, g, b] = lab_to_linear_rgb(l, a, b);
        from_linear_rgba(r, g, b, alpha)
    }

    pub fn from_lcha(l: f64, c: f64, h: f64, alpha: f64) -> [f64; 4] {
        from_laba(l, c * h.cos(), c * h.sin(), alpha)
    }

    pub fn from_xyz_d50(x: f64, y: f64, z: f64, alpha: f64) -> [f64; 4] {
        let [r, g, b] = xyz_d50_to_linear_rgb(x, y, z);
        from_linear_rgba(r, g, b, alpha)
    }

    pub fn from_xyz_d65(x: f64, y: f64, z: f64, alpha: f64) -> [f64; 4] {
        let [r, g, b] = xyz_d65_to_linear_rgb(x, y, z);
        from_linear_rgba(r, g, b, alpha)
    }
}

// ---------------------------------------------------------------------
// Color spaces: syntax, variable environment, result construction.
// ---------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Space {
    Rgb,
    Hsl,
    Hwb,
    Hsv,
    Lab,
    Lch,
    Oklab,
    Oklch,
    Srgb,
    SrgbLinear,
    XyzD50,
    XyzD65,
}

impl Space {
    const ALL: [Space; 12] = [
        Space::Rgb,
        Space::Hsl,
        Space::Hwb,
        Space::Hsv,
        Space::Lab,
        Space::Lch,
        Space::Oklab,
        Space::Oklch,
        Space::Srgb,
        Space::SrgbLinear,
        Space::XyzD50,
        Space::XyzD65,
    ];

    fn fn_name(self) -> &'static str {
        match self {
            Space::Rgb => "rgb",
            Space::Hsl => "hsl",
            Space::Hwb => "hwb",
            Space::Hsv => "hsv",
            Space::Lab => "lab",
            Space::Lch => "lch",
            Space::Oklab => "oklab",
            Space::Oklch => "oklch",
            Space::Srgb | Space::SrgbLinear | Space::XyzD50 | Space::XyzD65 => "color",
        }
    }

    /// The `<color-space>` token of `color(...)`, if any.
    fn profile(self) -> Option<&'static str> {
        match self {
            Space::Srgb => Some("srgb"),
            Space::SrgbLinear => Some("srgb-linear"),
            Space::XyzD50 => Some("xyz-d50"),
            Space::XyzD65 => Some("xyz-d65"),
            _ => None,
        }
    }

    /// Component variable names in relative color syntax.
    fn var_names(self) -> [&'static str; 4] {
        match self {
            Space::Rgb | Space::Srgb | Space::SrgbLinear => ["r", "g", "b", "alpha"],
            Space::Hsl => ["h", "s", "l", "alpha"],
            Space::Hwb => ["h", "w", "b", "alpha"],
            Space::Hsv => ["h", "s", "v", "alpha"],
            Space::Lab | Space::Oklab => ["l", "a", "b", "alpha"],
            Space::Lch | Space::Oklch => ["l", "c", "h", "alpha"],
            Space::XyzD50 | Space::XyzD65 => ["x", "y", "z", "alpha"],
        }
    }

    /// The variable environment of a relative color, in the units used by
    /// the relative syntax (e.g. rgb: 0..255, hsl s/l: 0..100, hue: degrees).
    ///
    /// Mirrors the library's `to_*` conversions, including which of them
    /// clamp the rgb components and/or alpha.
    fn var_env(self, rgba: [f64; 4]) -> [f64; 4] {
        use reference::*;
        let [r, g, b, a] = rgba;
        let cl = |v: f64| v.clamp(0.0, 1.0);
        match self {
            Space::Rgb => [r * 255.0, g * 255.0, b * 255.0, a],
            Space::Hsl => {
                let [h, s, l] = rgb_to_hsl(cl(r), cl(g), cl(b));
                [h, s * 100.0, l * 100.0, cl(a)]
            }
            Space::Hwb => {
                let [h, w, bl] = rgb_to_hwb(cl(r), cl(g), cl(b));
                [h, w * 100.0, bl * 100.0, cl(a)]
            }
            Space::Hsv => {
                let [h, s, v] = rgb_to_hsv(cl(r), cl(g), cl(b));
                [h, s * 100.0, v * 100.0, cl(a)]
            }
            Space::Lab => {
                let [l, aa, bb] = linear_rgb_to_lab(to_linear(r), to_linear(g), to_linear(b));
                [l, aa, bb, cl(a)]
            }
            Space::Lch => {
                let [l, aa, bb] = linear_rgb_to_lab(to_linear(r), to_linear(g), to_linear(b));
                let c = (aa * aa + bb * bb).sqrt();
                [l, c, bb.atan2(aa).to_degrees(), cl(a)]
            }
            Space::Oklab => {
                let [l, aa, bb] = linear_rgb_to_oklab(to_linear(r), to_linear(g), to_linear(b));
                [l, aa, bb, cl(a)]
            }
            Space::Oklch => {
                let [l, aa, bb] = linear_rgb_to_oklab(to_linear(r), to_linear(g), to_linear(b));
                let c = (aa * aa + bb * bb).sqrt();
                [l, c, bb.atan2(aa).to_degrees(), cl(a)]
            }
            Space::Srgb => [r, g, b, a],
            Space::SrgbLinear => [to_linear(r), to_linear(g), to_linear(b), a],
            Space::XyzD50 => {
                let [x, y, z] = linear_rgb_to_xyz_d50(to_linear(r), to_linear(g), to_linear(b));
                [x, y, z, a]
            }
            Space::XyzD65 => {
                let [x, y, z] = linear_rgb_to_xyz_d65(to_linear(r), to_linear(g), to_linear(b));
                [x, y, z, a]
            }
        }
    }

    /// Build the resulting rgba color from evaluated relative components.
    ///
    /// Mirrors the library's relative-color branches: per-component domain
    /// handling (e.g. `l.max(0)`, `c.max(0)`), hue in degrees converted to
    /// radians for lch/oklch, alpha passed through unclamped.
    fn build(self, v: [f64; 4]) -> [f64; 4] {
        use reference::*;
        let [c0, c1, c2, a] = v;
        match self {
            Space::Rgb => [c0 / 255.0, c1 / 255.0, c2 / 255.0, a],
            Space::Hsl => from_hsla(
                c0,
                (c1 / 100.0).clamp(0.0, 1.0),
                (c2 / 100.0).clamp(0.0, 1.0),
                a,
            ),
            Space::Hwb => from_hwba(c0, c1 / 100.0, c2 / 100.0, a),
            Space::Hsv => from_hsva(c0, c1 / 100.0, c2 / 100.0, a),
            Space::Lab => from_laba(c0.max(0.0), c1, c2, a),
            Space::Lch => from_lcha(c0.max(0.0), c1.max(0.0), c2.to_radians(), a),
            Space::Oklab => from_oklaba(c0.max(0.0), c1, c2, a),
            Space::Oklch => from_oklcha(c0.max(0.0), c1.max(0.0), c2.to_radians(), a),
            Space::Srgb => [c0, c1, c2, a],
            Space::SrgbLinear => from_linear_rgba(c0, c1, c2, a),
            Space::XyzD50 => from_xyz_d50(c0, c1, c2, a),
            Space::XyzD65 => from_xyz_d65(c0, c1, c2, a),
        }
    }
}

// ---------------------------------------------------------------------
// Syntax tree: absolute colors
// ---------------------------------------------------------------------

#[derive(Debug, Clone, Copy)]
enum AngleUnit {
    Deg,
    Grad,
    Rad,
    Turn,
    Bare,
}

#[derive(Debug, Clone, Copy)]
enum AbsComp {
    /// `none` (missing component).
    None,
    /// Bare number, in the component's own scale.
    Num(f64),
    /// Percentage, e.g. `Pct(50.0)` is `50%`.
    Pct(f64),
    /// Angle with unit (hue positions only).
    Angle(f64, AngleUnit),
}

impl AbsComp {
    fn css(&self) -> String {
        match *self {
            AbsComp::None => "none".into(),
            AbsComp::Num(v) => fmt_num(v),
            AbsComp::Pct(v) => format!("{}%", fmt_num(v)),
            AbsComp::Angle(v, AngleUnit::Deg) => format!("{}deg", fmt_num(v)),
            AbsComp::Angle(v, AngleUnit::Grad) => format!("{}grad", fmt_num(v)),
            AbsComp::Angle(v, AngleUnit::Rad) => format!("{}rad", fmt_num(v)),
            AbsComp::Angle(v, AngleUnit::Turn) => format!("{}turn", fmt_num(v)),
            AbsComp::Angle(v, AngleUnit::Bare) => fmt_num(v),
        }
    }

    /// Mirrors `parse_percent_or_float`: `none` -> (0, false),
    /// percentage -> (v/100, true), number -> (v, false).
    fn pct_or_float(&self) -> (f64, bool) {
        match *self {
            AbsComp::None => (0.0, false),
            AbsComp::Num(v) => (v, false),
            AbsComp::Pct(v) => (v / 100.0, true),
            AbsComp::Angle(..) => panic!("angle in number/percent position"),
        }
    }

    /// Mirrors `parse_angle`: result in degrees, `none` -> 0.
    fn angle_deg(&self) -> f64 {
        match *self {
            AbsComp::None => 0.0,
            AbsComp::Num(v) => v,
            AbsComp::Angle(v, AngleUnit::Deg) | AbsComp::Angle(v, AngleUnit::Bare) => v,
            AbsComp::Angle(v, AngleUnit::Grad) => v * 360.0 / 400.0,
            AbsComp::Angle(v, AngleUnit::Rad) => v.to_degrees(),
            AbsComp::Angle(v, AngleUnit::Turn) => v * 360.0,
            AbsComp::Pct(..) => panic!("percent in angle position"),
        }
    }
}

/// Shortest-roundtrip formatting; generated values have at most 2 decimals.
fn fmt_num(v: f64) -> String {
    format!("{v}")
}

/// Mirrors `FloatFmt` for values on the 0.001 grid.
fn fmt_float(v: f64) -> String {
    let rounded = (v * 1000.0).round() / 1000.0;
    let fract = (rounded.abs().fract() * 1000.0).round() as u32;
    let precision = if fract == 0 {
        0
    } else if fract % 10 != 0 {
        3
    } else if fract % 100 != 0 {
        2
    } else {
        1
    };
    format!("{rounded:.precision$}")
}

fn gen_num(rng: &mut Rng, lo: f64, hi: f64) -> f64 {
    // Two decimals keep the generated strings simple and exactly parseable.
    (rng.range(lo, hi) * 100.0).round() / 100.0
}

fn gen_pct_or_num(rng: &mut Rng, num: (f64, f64), pct: (f64, f64)) -> AbsComp {
    match rng.below(10) {
        0 => AbsComp::None,
        1..=4 => AbsComp::Num(gen_num(rng, num.0, num.1)),
        _ => AbsComp::Pct(gen_num(rng, pct.0, pct.1)),
    }
}

fn gen_hue(rng: &mut Rng) -> AbsComp {
    match rng.below(10) {
        0 => AbsComp::None,
        1 => AbsComp::Angle(gen_num(rng, -370.0, 730.0), AngleUnit::Bare),
        _ => {
            let (v, u) = match rng.below(4) {
                0 => (gen_num(rng, -370.0, 730.0), AngleUnit::Deg),
                1 => (gen_num(rng, -410.0, 810.0), AngleUnit::Grad),
                2 => (gen_num(rng, -6.5, 12.5), AngleUnit::Rad),
                _ => (gen_num(rng, -1.1, 2.1), AngleUnit::Turn),
            };
            AbsComp::Angle(v, u)
        }
    }
}

fn gen_abs_components(rng: &mut Rng, space: Space) -> [AbsComp; 3] {
    let pn = |rng: &mut Rng, num: (f64, f64), pct: (f64, f64)| gen_pct_or_num(rng, num, pct);
    match space {
        Space::Rgb => [
            pn(rng, (-30.0, 290.0), (-10.0, 110.0)),
            pn(rng, (-30.0, 290.0), (-10.0, 110.0)),
            pn(rng, (-30.0, 290.0), (-10.0, 110.0)),
        ],
        Space::Hsl => [
            gen_hue(rng),
            pn(rng, (-10.0, 110.0), (-10.0, 110.0)),
            pn(rng, (-10.0, 110.0), (-10.0, 110.0)),
        ],
        Space::Hwb => [
            gen_hue(rng),
            pn(rng, (-10.0, 110.0), (-10.0, 110.0)),
            pn(rng, (-10.0, 110.0), (-10.0, 110.0)),
        ],
        Space::Hsv => [
            gen_hue(rng),
            pn(rng, (-10.0, 110.0), (-10.0, 110.0)),
            pn(rng, (-10.0, 110.0), (-10.0, 110.0)),
        ],
        Space::Lab => [
            pn(rng, (-10.0, 110.0), (-10.0, 110.0)),
            pn(rng, (-140.0, 140.0), (-110.0, 110.0)),
            pn(rng, (-140.0, 140.0), (-110.0, 110.0)),
        ],
        Space::Lch => [
            pn(rng, (-10.0, 110.0), (-10.0, 110.0)),
            pn(rng, (-20.0, 160.0), (-10.0, 110.0)),
            gen_hue(rng),
        ],
        Space::Oklab => [
            pn(rng, (-0.2, 1.2), (-10.0, 110.0)),
            pn(rng, (-0.5, 0.5), (-110.0, 110.0)),
            pn(rng, (-0.5, 0.5), (-110.0, 110.0)),
        ],
        Space::Oklch => [
            pn(rng, (-0.2, 1.2), (-10.0, 110.0)),
            pn(rng, (-0.1, 0.5), (-10.0, 110.0)),
            gen_hue(rng),
        ],
        Space::Srgb | Space::SrgbLinear | Space::XyzD50 | Space::XyzD65 => [
            pn(rng, (-0.3, 1.3), (-10.0, 110.0)),
            pn(rng, (-0.3, 1.3), (-10.0, 110.0)),
            pn(rng, (-0.3, 1.3), (-10.0, 110.0)),
        ],
    }
}

fn gen_abs_alpha(rng: &mut Rng) -> Option<AbsComp> {
    match rng.below(10) {
        0..=4 => None,
        5..=6 => Some(AbsComp::Num(gen_num(rng, -0.2, 1.2))),
        7..=8 => Some(AbsComp::Pct(gen_num(rng, -10.0, 110.0))),
        _ => Some(AbsComp::None),
    }
}

fn abs_string(space: Space, comps: &[AbsComp; 3], alpha: &Option<AbsComp>) -> String {
    let a = alpha.map_or_else(String::new, |a| format!(" / {}", a.css()));
    match space.profile() {
        Some(p) => format!(
            "color({p} {} {} {}{a})",
            comps[0].css(),
            comps[1].css(),
            comps[2].css()
        ),
        None => format!(
            "{}({} {} {}{a})",
            space.fn_name(),
            comps[0].css(),
            comps[1].css(),
            comps[2].css()
        ),
    }
}

/// Independently compute the rgba value of an absolute color, mirroring the
/// library's per-component domains: percentages are rescaled per component,
/// alpha is clamped to [0,1], `none` is 0, and out-of-range components are
/// clamped exactly where the library clamps them.
fn eval_absolute(space: Space, comps: &[AbsComp; 3], alpha: Option<AbsComp>) -> [f64; 4] {
    use reference::*;
    let alpha = alpha.map_or(1.0, |c| c.pct_or_float().0.clamp(0.0, 1.0));
    let [c0, c1, c2] = *comps;
    match space {
        Space::Rgb => {
            let f = |c: AbsComp| {
                let (v, pct) = c.pct_or_float();
                (if pct { v } else { v / 255.0 }).clamp(0.0, 1.0)
            };
            [f(c0), f(c1), f(c2), alpha]
        }
        Space::Hsl => {
            let f = |c: AbsComp| {
                let (v, pct) = c.pct_or_float();
                if pct { v } else { v / 100.0 }
            };
            from_hsla(c0.angle_deg(), f(c1), f(c2), alpha)
        }
        Space::Hwb => {
            let f = |c: AbsComp| {
                let (v, pct) = c.pct_or_float();
                if pct { v } else { v / 100.0 }
            };
            from_hwba(c0.angle_deg(), f(c1), f(c2), alpha)
        }
        Space::Hsv => {
            let f = |c: AbsComp| {
                let (v, pct) = c.pct_or_float();
                if pct { v } else { v / 100.0 }
            };
            from_hsva(c0.angle_deg(), f(c1), f(c2), alpha)
        }
        Space::Lab => {
            let l = {
                let (v, pct) = c0.pct_or_float();
                if pct { v * 100.0 } else { v }
            };
            let ab = |c: AbsComp| {
                let (v, pct) = c.pct_or_float();
                // 100% <-> +/-125
                if pct { v * 125.0 } else { v }
            };
            from_laba(l.max(0.0), ab(c1), ab(c2), alpha)
        }
        Space::Lch => {
            let l = {
                let (v, pct) = c0.pct_or_float();
                if pct { v * 100.0 } else { v }
            };
            let c = {
                let (v, pct) = c1.pct_or_float();
                // 100% <-> 150
                if pct { v * 150.0 } else { v }
            };
            from_lcha(l.max(0.0), c.max(0.0), c2.angle_deg().to_radians(), alpha)
        }
        Space::Oklab => {
            // The percent flag is ignored for L: 50% and 0.5 are the same.
            let l = c0.pct_or_float().0;
            let ab = |c: AbsComp| {
                let (v, pct) = c.pct_or_float();
                // 100% <-> +/-0.4
                if pct { v * 0.4 } else { v }
            };
            from_oklaba(l.max(0.0), ab(c1), ab(c2), alpha)
        }
        Space::Oklch => {
            let l = c0.pct_or_float().0;
            let c = {
                let (v, pct) = c1.pct_or_float();
                // 100% <-> 0.4
                if pct { v * 0.4 } else { v }
            };
            from_oklcha(l.max(0.0), c.max(0.0), c2.angle_deg().to_radians(), alpha)
        }
        Space::Srgb => {
            // `color(srgb ...)` components are stored raw (no clamping).
            [
                c0.pct_or_float().0,
                c1.pct_or_float().0,
                c2.pct_or_float().0,
                alpha,
            ]
        }
        Space::SrgbLinear => from_linear_rgba(
            c0.pct_or_float().0,
            c1.pct_or_float().0,
            c2.pct_or_float().0,
            alpha,
        ),
        Space::XyzD50 => from_xyz_d50(
            c0.pct_or_float().0,
            c1.pct_or_float().0,
            c2.pct_or_float().0,
            alpha,
        ),
        Space::XyzD65 => from_xyz_d65(
            c0.pct_or_float().0,
            c1.pct_or_float().0,
            c2.pct_or_float().0,
            alpha,
        ),
    }
}

// ---------------------------------------------------------------------
// Syntax tree: relative colors
// ---------------------------------------------------------------------

#[derive(Debug, Clone)]
enum CalcExpr {
    Num(f64),
    Var(usize),
    Bin(u8, Box<CalcExpr>, Box<CalcExpr>),
}

impl CalcExpr {
    fn eval(&self, env: &[f64; 4]) -> f64 {
        match self {
            CalcExpr::Num(v) => *v,
            CalcExpr::Var(i) => env[*i],
            CalcExpr::Bin(op, a, b) => {
                let (x, y) = (a.eval(env), b.eval(env));
                match op {
                    b'+' => x + y,
                    b'-' => x - y,
                    b'*' => x * y,
                    b'/' => x / y,
                    _ => unreachable!(),
                }
            }
        }
    }

    fn css(&self, names: &[&str; 4]) -> String {
        match self {
            CalcExpr::Num(v) => fmt_num(*v),
            CalcExpr::Var(i) => names[*i].into(),
            CalcExpr::Bin(op, a, b) => {
                format!("({} {} {})", a.css(names), *op as char, b.css(names))
            }
        }
    }

    fn uses_var(&self, idx: usize) -> bool {
        match self {
            CalcExpr::Num(_) => false,
            CalcExpr::Var(i) => *i == idx,
            CalcExpr::Bin(_, a, b) => a.uses_var(idx) || b.uses_var(idx),
        }
    }
}

/// Literals used inside `calc()`; plain decimal, exactly parseable.
const CALC_NUMS: [f64; 11] = [0.1, 0.25, 0.5, 0.75, 1.5, 2.0, 3.0, 4.0, 10.0, 25.0, 100.0];
const CALC_DIVISORS: [f64; 5] = [0.25, 0.5, 2.0, 4.0, 10.0];

/// Generates `calc(A op B)` where A and B are numbers, variables or
/// parenthesized sub-expressions -- exactly the grammar the parser accepts.
fn gen_calc(rng: &mut Rng, depth: usize) -> CalcExpr {
    fn operand(rng: &mut Rng, depth: usize) -> CalcExpr {
        if depth > 0 && rng.chance(0.4) {
            gen_calc(rng, depth - 1)
        } else if rng.chance(0.5) {
            CalcExpr::Num(CALC_NUMS[rng.below(CALC_NUMS.len())])
        } else {
            CalcExpr::Var(rng.below(4))
        }
    }
    let op = b"+-*/"[rng.below(4)];
    let lhs = operand(rng, depth);
    // Division always gets a nonzero literal divisor (div-by-zero is a parse error).
    let rhs = if op == b'/' {
        CalcExpr::Num(CALC_DIVISORS[rng.below(CALC_DIVISORS.len())])
    } else {
        operand(rng, depth)
    };
    CalcExpr::Bin(op, Box::new(lhs), Box::new(rhs))
}

#[derive(Debug, Clone)]
enum RelComp {
    /// `none` (missing component; the library resolves it to 0.0).
    None,
    /// Component keyword inherited from the origin color.
    Var(usize),
    /// Bare number in the component's relative-syntax scale.
    Num(f64),
    /// `calc(...)` expression.
    Calc(CalcExpr),
}

impl RelComp {
    fn eval(&self, env: &[f64; 4]) -> f64 {
        match self {
            RelComp::None => 0.0,
            RelComp::Var(i) => env[*i],
            RelComp::Num(v) => *v,
            RelComp::Calc(e) => e.eval(env),
        }
    }

    fn css(&self, names: &[&str; 4]) -> String {
        match self {
            RelComp::None => "none".into(),
            RelComp::Var(i) => names[*i].into(),
            RelComp::Num(v) => fmt_num(*v),
            RelComp::Calc(e) => format!("calc{}", e.css(names)),
        }
    }

    fn uses_var(&self, idx: usize) -> bool {
        match self {
            RelComp::None | RelComp::Num(_) => false,
            RelComp::Var(i) => *i == idx,
            RelComp::Calc(e) => e.uses_var(idx),
        }
    }
}

/// Range of bare numbers per component, in relative-syntax units.
fn rel_num_range(space: Space, idx: usize) -> (f64, f64) {
    match space {
        Space::Rgb => (-50.0, 300.0),
        Space::Hsl | Space::Hwb | Space::Hsv => {
            if idx == 0 {
                (-360.0, 720.0)
            } else {
                (-20.0, 120.0)
            }
        }
        Space::Lab => {
            if idx == 0 {
                (-10.0, 110.0)
            } else {
                (-140.0, 140.0)
            }
        }
        Space::Lch => match idx {
            0 => (-10.0, 110.0),
            1 => (-20.0, 160.0),
            _ => (-360.0, 720.0),
        },
        Space::Oklab => {
            if idx == 0 {
                (-0.2, 1.2)
            } else {
                (-0.5, 0.5)
            }
        }
        Space::Oklch => match idx {
            0 => (-0.2, 1.2),
            1 => (-0.1, 0.5),
            _ => (-360.0, 720.0),
        },
        Space::Srgb | Space::SrgbLinear | Space::XyzD50 | Space::XyzD65 => (-0.3, 1.3),
    }
}

fn gen_rel_comp(rng: &mut Rng, space: Space, idx: usize) -> RelComp {
    match rng.below(20) {
        0..=5 => RelComp::Var(rng.below(4)),
        6..=7 => RelComp::Calc(gen_calc(rng, 2)),
        8..=9 => RelComp::None,
        _ => {
            let (lo, hi) = rel_num_range(space, idx);
            RelComp::Num(gen_num(rng, lo, hi))
        }
    }
}

fn gen_rel_alpha(rng: &mut Rng) -> Option<RelComp> {
    match rng.below(10) {
        // No slash: alpha is inherited from the origin color.
        0..=2 => None,
        _ => Some(match rng.below(20) {
            0..=5 => RelComp::Var(3),
            6..=7 => RelComp::Calc(gen_calc(rng, 2)),
            8..=9 => RelComp::None,
            _ => RelComp::Num(gen_num(rng, -0.2, 1.2)),
        }),
    }
}

fn rel_string(space: Space, origin: &str, comps: &[RelComp; 3], alpha: &Option<RelComp>) -> String {
    let names = space.var_names();
    let a = alpha
        .as_ref()
        .map_or_else(String::new, |a| format!(" / {}", a.css(&names)));
    match space.profile() {
        Some(p) => format!(
            "color(from {origin} {p} {} {} {}{a})",
            comps[0].css(&names),
            comps[1].css(&names),
            comps[2].css(&names)
        ),
        None => format!(
            "{}(from {origin} {} {} {}{a})",
            space.fn_name(),
            comps[0].css(&names),
            comps[1].css(&names),
            comps[2].css(&names)
        ),
    }
}

// ---------------------------------------------------------------------
// Comparison helpers
// ---------------------------------------------------------------------

#[track_caller]
fn close(got: f64, want: f64, tol: f64, what: &str, ctx: &str) {
    let err = (got - want).abs();
    assert!(
        err <= tol,
        "{what}: got {got}, want {want}, err {err} > tol {tol}\nctx: {ctx}"
    );
}

/// Tolerance for a full rgba comparison after f32 evaluation: absolute
/// floor plus a relative part for out-of-gamut components.
#[track_caller]
fn assert_rgba_close(got: [f64; 4], want: [f64; 4], abs_tol: f64, ctx: &str) {
    for (i, (&g, &w)) in got.iter().zip(want.iter()).enumerate() {
        let tol = abs_tol + 1e-3 * w.abs();
        close(g, w, tol, &format!("rgba[{i}]"), ctx);
    }
}

/// Is Oklab's inverse transform well-conditioned at this color? Near
/// `S_LMS = 0` (and for dark colors in general) `cbrt` amplifies the
/// f32 rounding of stored rgb components by orders of magnitude.
fn oklab_well_conditioned(rgba: [f64; 4]) -> bool {
    let lin = [
        reference::to_linear(rgba[0]),
        reference::to_linear(rgba[1]),
        reference::to_linear(rgba[2]),
    ];
    let [_, _, s] = reference::linear_rgb_to_lms(lin[0], lin[1], lin[2]);
    let l = reference::linear_rgb_to_oklab(lin[0], lin[1], lin[2])[0];
    l >= 0.3 && s.abs() >= 1e-4
}

/// Signed circular difference of two angles in degrees, in [-180, 180).
fn angle_diff_deg(a: f64, b: f64) -> f64 {
    ((a - b) % 360.0 + 540.0) % 360.0 - 180.0
}

/// Signed circular difference of two angles in radians, in [-pi, pi).
fn angle_diff_rad(a: f64, b: f64) -> f64 {
    ((a - b) % TAU + 3.0 * PI) % TAU - PI
}

// ---------------------------------------------------------------------
// Property: generated absolute colors evaluate to the reference value
// ---------------------------------------------------------------------

#[test]
fn absolute_parse_matches_reference() {
    let mut rng = Rng::new(0x5EED_A001);
    for _ in 0..400 {
        let space = Space::ALL[rng.below(Space::ALL.len())];
        let comps = gen_abs_components(&mut rng, space);
        let alpha = gen_abs_alpha(&mut rng);
        let s = abs_string(space, &comps, &alpha);
        let want = eval_absolute(space, &comps, alpha);
        let c = parse(&s).unwrap_or_else(|e| panic!("parse failed for {s:?}: {e}"));
        assert_rgba_close(
            [c.r as f64, c.g as f64, c.b as f64, c.a as f64],
            want,
            2e-3,
            &s,
        );
    }
}

// ---------------------------------------------------------------------
// Property: generated relative colors evaluate to the reference value
// ---------------------------------------------------------------------

#[test]
fn relative_parse_matches_reference() {
    let mut rng = Rng::new(0x5EED_B002);
    for _ in 0..400 {
        // Origin color: any absolute color (may itself contain `none`,
        // percentages, out-of-gamut components, ...).
        let origin_space = Space::ALL[rng.below(Space::ALL.len())];
        let origin_comps = gen_abs_components(&mut rng, origin_space);
        let origin_alpha = gen_abs_alpha(&mut rng);
        let origin_str = abs_string(origin_space, &origin_comps, &origin_alpha);
        let origin_rgba = eval_absolute(origin_space, &origin_comps, origin_alpha);

        let space = Space::ALL[rng.below(Space::ALL.len())];
        let env = space.var_env(origin_rgba);

        // The oklab/oklch variable environment comes from Oklab's inverse
        // transform; note whether it is well-conditioned at the origin.
        let oklab_cond_ok = oklab_well_conditioned(origin_rgba);

        let mut comps = [
            gen_rel_comp(&mut rng, space, 0),
            gen_rel_comp(&mut rng, space, 1),
            gen_rel_comp(&mut rng, space, 2),
        ];
        let mut alpha = gen_rel_alpha(&mut rng);

        // For lch/oklch targets, the inherited hue of a near-achromatic
        // origin is `atan2` noise (f32 and f64 disagree wildly). A hue
        // variable is only meaningful when the origin's chroma is
        // significant and well-conditioned; otherwise replace components
        // that reference it with plain numbers.
        let hue_env_ok = match space {
            Space::Lch => env[1] >= 0.5,
            Space::Oklch => env[1] >= 0.05 && oklab_cond_ok,
            _ => true,
        };
        if !hue_env_ok {
            for (i, c) in comps.iter_mut().enumerate() {
                if c.uses_var(2) {
                    let (lo, hi) = rel_num_range(space, i);
                    *c = RelComp::Num(gen_num(&mut rng, lo, hi));
                }
            }
            if alpha.as_ref().is_some_and(|a| a.uses_var(2)) {
                alpha = Some(RelComp::Num(gen_num(&mut rng, -0.2, 1.2)));
            }
        }

        let s = rel_string(space, &origin_str, &comps, &alpha);

        let mut vals = [0.0; 4];
        for (i, c) in comps.iter().enumerate() {
            vals[i] = c.eval(&env);
        }
        vals[3] = alpha.as_ref().map_or(env[3], |c| c.eval(&env));
        let want = space.build(vals);

        // Where the oklab/oklch environment is ill-conditioned, the f32
        // and f64 environments diverge by a few 1e-3 and the result
        // inherits that; relax the tolerance accordingly.
        let tol = if matches!(space, Space::Oklab | Space::Oklch) && !oklab_cond_ok {
            2e-2
        } else {
            2e-3
        };

        let c = parse(&s).unwrap_or_else(|e| panic!("parse failed for {s:?}: {e}"));
        assert_rgba_close(
            [c.r as f64, c.g as f64, c.b as f64, c.a as f64],
            want,
            tol,
            &s,
        );
    }
}

// ---------------------------------------------------------------------
// Property 2: conversions vs. the f64 reference, each color space with
// its own error definition.
// ---------------------------------------------------------------------

#[test]
fn conversions_match_f64_reference() {
    use reference::*;
    let mut rng = Rng::new(0x5EED_C003);
    for _ in 0..300 {
        // Mix in-gamut and out-of-gamut (cross-gamut) colors.
        let (lo, hi) = if rng.chance(0.3) {
            (-0.25, 1.25)
        } else {
            (0.0, 1.0)
        };
        let c = Color::new(
            rng.range(lo, hi) as f32,
            rng.range(lo, hi) as f32,
            rng.range(lo, hi) as f32,
            rng.range(-0.2, 1.2) as f32,
        );
        let [r, g, b, a] = [c.r as f64, c.g as f64, c.b as f64, c.a as f64];
        let ctx = format!("color={c:?}");
        let lin = [to_linear(r), to_linear(g), to_linear(b)];

        // Linear sRGB: components in [0,1] (unclamped), alpha passed through.
        {
            let got = c.to_linear_rgba();
            let want = [lin[0], lin[1], lin[2], a];
            for (i, (&g_, &w_)) in got.iter().zip(want.iter()).enumerate() {
                close(g_ as f64, w_, 1e-5, &format!("to_linear_rgba[{i}]"), &ctx);
            }
        }

        // CIE XYZ (D65 and D50): components in [0,1], alpha passed through raw.
        for (got, want) in [
            (
                c.to_xyz_d65(),
                linear_rgb_to_xyz_d65(lin[0], lin[1], lin[2]),
            ),
            (
                c.to_xyz_d50(),
                linear_rgb_to_xyz_d50(lin[0], lin[1], lin[2]),
            ),
        ] {
            for i in 0..3 {
                close(got[i] as f64, want[i], 1e-5, &format!("to_xyz[{i}]"), &ctx);
            }
            close(got[3] as f64, a, 1e-6, "to_xyz alpha", &ctx);
        }

        // Oklab: L in [0,1], a/b in [-0.4,0.4]; alpha clamped to [0,1].
        // The inverse transform is ill-conditioned for dark colors and
        // near the S_LMS = 0 plane (cbrt amplifies f32 rounding by
        // orders of magnitude), so the a/b tolerance follows the
        // conditioning instead of using one global value.
        {
            let want = linear_rgb_to_oklab(lin[0], lin[1], lin[2]);
            let well = oklab_well_conditioned([r, g, b, a]);
            let ab_tol = if well { 1e-4 } else { 1e-2 };
            let got = c.to_oklaba();
            close(got[0] as f64, want[0], 1e-4, "to_oklaba l", &ctx);
            close(got[1] as f64, want[1], ab_tol, "to_oklaba a", &ctx);
            close(got[2] as f64, want[2], ab_tol, "to_oklaba b", &ctx);
            close(
                got[3] as f64,
                a.clamp(0.0, 1.0),
                1e-6,
                "to_oklaba alpha",
                &ctx,
            );

            // Oklch: chroma like Oklab a/b; hue is circular and only
            // compared when the chroma is significant and the transform
            // is well-conditioned (hue is ill-defined near c = 0).
            let wc = (want[1] * want[1] + want[2] * want[2]).sqrt();
            let wh = want[2].atan2(want[1]);
            let got = c.to_oklcha();
            close(got[0] as f64, want[0], 1e-4, "to_oklcha l", &ctx);
            close(got[1] as f64, wc, ab_tol, "to_oklcha c", &ctx);
            if well && wc > 0.05 {
                let err = angle_diff_rad(got[2] as f64, wh).abs();
                assert!(err <= 1e-2, "to_oklcha h: err {err} rad\nctx: {ctx}");
            }
            close(
                got[3] as f64,
                a.clamp(0.0, 1.0),
                1e-6,
                "to_oklcha alpha",
                &ctx,
            );
        }

        // CIE Lab: L in [0,100] -- 100x the scale of Oklab L, so its
        // tolerance is 100x as well; a/b are larger still.
        {
            let want = linear_rgb_to_lab(lin[0], lin[1], lin[2]);
            let got = c.to_laba();
            close(got[0] as f64, want[0], 0.01, "to_laba L", &ctx);
            close(got[1] as f64, want[1], 0.02, "to_laba a", &ctx);
            close(got[2] as f64, want[2], 0.02, "to_laba b", &ctx);
            close(
                got[3] as f64,
                a.clamp(0.0, 1.0),
                1e-6,
                "to_laba alpha",
                &ctx,
            );
        }

        // LCH: hue circular, only when chroma (Lab units) is significant.
        {
            let [wl, wa, wb] = linear_rgb_to_lab(lin[0], lin[1], lin[2]);
            let wc = (wa * wa + wb * wb).sqrt();
            let wh = wb.atan2(wa);
            let got = c.to_lcha();
            close(got[0] as f64, wl, 0.01, "to_lcha L", &ctx);
            close(got[1] as f64, wc, 0.02, "to_lcha c", &ctx);
            if wc > 1.0 {
                let err = angle_diff_rad(got[2] as f64, wh).abs();
                assert!(err <= 1e-2, "to_lcha h: err {err} rad\nctx: {ctx}");
            }
            close(
                got[3] as f64,
                a.clamp(0.0, 1.0),
                1e-6,
                "to_lcha alpha",
                &ctx,
            );
        }

        // HSL / HSV / HWB: rgb clamped first; hue in degrees, circular,
        // only compared for chromatic colors (d = max - min not tiny).
        {
            let (cr, cg, cb) = (r.clamp(0.0, 1.0), g.clamp(0.0, 1.0), b.clamp(0.0, 1.0));
            let d = cr.max(cg.max(cb)) - cr.min(cg.min(cb));

            let got = c.to_hsla();
            let want = rgb_to_hsl(cr, cg, cb);
            if d > 1e-3 {
                let err = angle_diff_deg(got[0] as f64, want[0]).abs();
                assert!(err <= 0.05, "to_hsla h: err {err} deg\nctx: {ctx}");
            }
            close(got[1] as f64, want[1], 1e-5, "to_hsla s", &ctx);
            close(got[2] as f64, want[2], 1e-5, "to_hsla l", &ctx);
            close(
                got[3] as f64,
                a.clamp(0.0, 1.0),
                1e-6,
                "to_hsla alpha",
                &ctx,
            );

            let got = c.to_hsva();
            let want = rgb_to_hsv(cr, cg, cb);
            if d > 1e-3 {
                let err = angle_diff_deg(got[0] as f64, want[0]).abs();
                assert!(err <= 0.05, "to_hsva h: err {err} deg\nctx: {ctx}");
            }
            close(got[1] as f64, want[1], 1e-5, "to_hsva s", &ctx);
            close(got[2] as f64, want[2], 1e-5, "to_hsva v", &ctx);
            close(
                got[3] as f64,
                a.clamp(0.0, 1.0),
                1e-6,
                "to_hsva alpha",
                &ctx,
            );

            let got = c.to_hwba();
            let want = rgb_to_hwb(cr, cg, cb);
            if d > 1e-3 {
                let err = angle_diff_deg(got[0] as f64, want[0]).abs();
                assert!(err <= 0.05, "to_hwba h: err {err} deg\nctx: {ctx}");
            }
            close(got[1] as f64, want[1], 1e-5, "to_hwba w", &ctx);
            close(got[2] as f64, want[2], 1e-5, "to_hwba b", &ctx);
            close(
                got[3] as f64,
                a.clamp(0.0, 1.0),
                1e-6,
                "to_hwba alpha",
                &ctx,
            );
        }
    }
}

// ---------------------------------------------------------------------
// Property 1: for representable inputs, parse -> serialize -> parse
// returns the same value (and the same string).
//
// Inputs are generated on each serializer's own quantization grid and
// restricted to well-conditioned regions (e.g. no hue near the 0/360
// wrap, no near-zero chroma), which is exactly the "representable" part
// of the input space.
// ---------------------------------------------------------------------

/// Random value on the 0.001 grid in [lo, hi].
fn grid3(rng: &mut Rng, lo: f64, hi: f64) -> f64 {
    (rng.range(lo, hi) * 1000.0).round() / 1000.0
}

/// Same, but never zero (a round-tripped tiny negative would print as `-0`).
fn grid3_nonzero(rng: &mut Rng, lo: f64, hi: f64) -> f64 {
    loop {
        let v = grid3(rng, lo, hi);
        if v.abs() >= 0.001 {
            return v;
        }
    }
}

/// Optional alpha in the serializer's format: omitted, or an integer
/// percentage below 100% (100% is omitted by the serializers).
fn gen_alpha_pct(rng: &mut Rng) -> String {
    if rng.chance(0.5) {
        String::new()
    } else {
        format!(" / {}%", rng.below(100))
    }
}

/// Is an oklab triple representable for a 0.001-grid round trip?
/// Oklab's inverse is ill-conditioned near the S_LMS = 0 plane (cbrt
/// amplifies f32 rounding), so colors too close to that plane are
/// excluded from the representable subset.
fn oklab_roundtrip_ok(l: f64, a: f64, b: f64) -> bool {
    let [r, g, b] = reference::oklab_to_linear_rgb(l, a, b);
    let [_, _, s] = reference::linear_rgb_to_lms(r, g, b);
    s.abs() >= 1e-4
}

#[track_caller]
fn check_roundtrip(s0: &str, serialize: impl Fn(&Color) -> String) {
    let c1 = parse(s0).unwrap_or_else(|e| panic!("parse failed for {s0:?}: {e}"));
    let s1 = serialize(&c1);
    assert_eq!(s1, s0, "serialize(parse(s0)) != s0");
    let c2 = parse(&s1).unwrap_or_else(|e| panic!("re-parse failed for {s1:?}: {e}"));
    let s2 = serialize(&c2);
    assert_eq!(s2, s1, "serialization is not a fixed point of {s0:?}");
}

#[test]
fn roundtrip_parse_serialize_parse() {
    let mut rng = Rng::new(0x5EED_8004);

    // Hex: 8-bit channels, alpha 8-bit (omitted when 0xff).
    for _ in 0..150 {
        let (r, g, b, a) = (
            rng.below(256) as u8,
            rng.below(256) as u8,
            rng.below(256) as u8,
            rng.below(256) as u8,
        );
        let s0 = if a == 0xff {
            format!("#{r:02x}{g:02x}{b:02x}")
        } else {
            format!("#{r:02x}{g:02x}{b:02x}{a:02x}")
        };
        check_roundtrip(&s0, |c| c.to_css_hex().to_string());
    }

    // rgb(): 8-bit channels, alpha integer percent.
    for _ in 0..150 {
        let (r, g, b) = (rng.below(256), rng.below(256), rng.below(256));
        let s0 = format!("rgb({r} {g} {b}{})", gen_alpha_pct(&mut rng));
        check_roundtrip(&s0, |c| c.to_css_rgb().to_string());
    }

    // hsl(): hue on the 0.001-degree grid away from the 0/360 wrap;
    // s/l integer percent, saturated and light enough that hue is
    // well-conditioned (s >= 20%, 20% <= l <= 80%).
    for _ in 0..150 {
        let h = grid3(&mut rng, 0.01, 359.99);
        let s = 20 + rng.below(81);
        let l = 20 + rng.below(61);
        let s0 = format!("hsl({} {s}% {l}%{})", fmt_float(h), gen_alpha_pct(&mut rng));
        check_roundtrip(&s0, |c| c.to_css_hsl().to_string());
    }

    // hwb(): w/b integer percent with w + b <= 80% (below 100% the hue
    // survives; at w + b >= 100% the color is gray and hue is powerless).
    for _ in 0..150 {
        let h = grid3(&mut rng, 0.01, 359.99);
        let w = rng.below(81);
        let b = rng.below(81 - w);
        let s0 = format!("hwb({} {w}% {b}%{})", fmt_float(h), gen_alpha_pct(&mut rng));
        check_roundtrip(&s0, |c| c.to_css_hwb().to_string());
    }

    // oklab(): all components on the 0.001 grid, a/b nonzero, lightness
    // and S_LMS distance inside the well-conditioned region.
    for _ in 0..150 {
        let (l, a, b) = loop {
            let l = grid3(&mut rng, 0.3, 0.999);
            let a = grid3_nonzero(&mut rng, -0.4, 0.4);
            let b = grid3_nonzero(&mut rng, -0.4, 0.4);
            if oklab_roundtrip_ok(l, a, b) {
                break (l, a, b);
            }
        };
        let s0 = format!(
            "oklab({} {} {}{})",
            fmt_float(l),
            fmt_float(a),
            fmt_float(b),
            gen_alpha_pct(&mut rng)
        );
        check_roundtrip(&s0, |c| c.to_css_oklab().to_string());
    }

    // oklch(): lightness and chroma large enough (and far enough from
    // S_LMS = 0) that the hue survives the round trip through rgb at
    // 0.001-degree precision.
    for _ in 0..150 {
        let (l, ch, h) = loop {
            let l = grid3(&mut rng, 0.4, 0.999);
            let ch = grid3(&mut rng, 0.2, 0.35);
            let h = grid3(&mut rng, 0.01, 359.99);
            let hr = h.to_radians();
            if oklab_roundtrip_ok(l, ch * hr.cos(), ch * hr.sin()) {
                break (l, ch, h);
            }
        };
        let s0 = format!(
            "oklch({} {} {}{})",
            fmt_float(l),
            fmt_float(ch),
            fmt_float(h),
            gen_alpha_pct(&mut rng)
        );
        check_roundtrip(&s0, |c| c.to_css_oklch().to_string());
    }

    // lab(): L in (0, 100), a/b nonzero, all on the 0.001 grid.
    for _ in 0..150 {
        let l = grid3(&mut rng, 0.001, 99.999);
        let a = grid3_nonzero(&mut rng, -125.0, 125.0);
        let b = grid3_nonzero(&mut rng, -125.0, 125.0);
        let s0 = format!(
            "lab({} {} {}{})",
            fmt_float(l),
            fmt_float(a),
            fmt_float(b),
            gen_alpha_pct(&mut rng)
        );
        check_roundtrip(&s0, |c| c.to_css_lab().to_string());
    }

    // lch(): chroma >= 10 (Lab units) keeps the hue well-conditioned.
    for _ in 0..150 {
        let l = grid3(&mut rng, 0.001, 99.999);
        let ch = grid3(&mut rng, 10.0, 150.0);
        let h = grid3(&mut rng, 0.01, 359.99);
        let s0 = format!(
            "lch({} {} {}{})",
            fmt_float(l),
            fmt_float(ch),
            fmt_float(h),
            gen_alpha_pct(&mut rng)
        );
        check_roundtrip(&s0, |c| c.to_css_lch().to_string());
    }

    // color(srgb ...): components on the 0.001 grid, including
    // out-of-gamut values, which this serializer preserves raw.
    for _ in 0..150 {
        let r = grid3_nonzero(&mut rng, -0.5, 1.5);
        let g = grid3_nonzero(&mut rng, -0.5, 1.5);
        let b = grid3_nonzero(&mut rng, -0.5, 1.5);
        let s0 = format!(
            "color(srgb {} {} {}{})",
            fmt_float(r),
            fmt_float(g),
            fmt_float(b),
            gen_alpha_pct(&mut rng)
        );
        check_roundtrip(&s0, |c| c.to_css_color_srgb().to_string());
    }
}
