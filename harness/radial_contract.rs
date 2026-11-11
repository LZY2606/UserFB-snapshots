//! Numeric regression contract for two-point conical (radial) gradients.
//!
//! Everything expected below is computed by an independent f64 reference:
//! pixel centers are mapped back into gradient space by an explicitly inverted
//! affine, the two-circle interpolation parameter is solved as a quadratic,
//! then spread mode, stop interpolation, premultiplication, the A8 mask and
//! SourceOver are evaluated in f64 and rounded once to bytes. No private
//! tiny-skia gradient/stage/matrix code is called; rendering goes exclusively
//! through the public `RadialGradient::new`, `Paint`, `Mask` and
//! `Pixmap::fill_path` API.
//!
//! Byte tolerance derivation (see `BYTE_TOL`):
//! 1. The library stores bytes by `round(clamp(f,0,1) * 255)`, so an exact
//!    real value always sits within 0.5 of an integer byte.
//! 2. On these small canvases every f32 pipeline operation stays well away
//!    from float precision limits: coordinate/root/color relative error is
//!    below roughly 1e-5, i.e. under 0.003 of a byte before the final round.
//! 3. The default (possibly lowp u16) pipeline adds at most one extra
//!    round-to-nearest on the 0..255 gradient channels (<= 0.5 byte).
//! Adding the two half-widths (0.5 final rounding + 0.5 intermediate rounding)
//! gives a maximum of one byte of difference on numerically stable pixels.
//! The threshold is therefore 1 and is never widened to accommodate results.
//! Pixels whose two-circle solution is float-unstable (both roots negative
//! with the larger one within `EDGE_T_MAX` of t = 0) carry no byte assertion at
//! all; genuinely invalid geometry (negative discriminant or roots far below
//! zero) must instead leave the opaque backdrop byte-for-byte untouched.
//!
//! Degenerate geometry this reference deliberately does NOT pin down:
//! * Identical centers with equal radii: the constructor rejects it, except in
//!   Pad mode where it collapses to an infinitely thin hard-stop ring
//!   (`RadialGradient::new` special case); no interpolated t exists there.
//! * The simple zero-focal-radius fast path
//!   (`new_radial_unchecked`, concentric with start radius 0) and the
//!   focal-on-circle stage: all off-center cases here keep both radii positive,
//!   so the native-focal and focal-on-circle pipelines are not exercised.
//! * Single-stop gradients (reduced to a solid color), empty stop lists and
//!   non-invertible shader transforms, all of which the API rejects or replaces
//!   before any pixel is evaluated.
//! * Pixels landing exactly on a cone tangent (discriminant 0), exactly on the
//!   t = 0 / t = 1 cone circles or exactly on the repeated 0.35 hard stop:
//!   f32 rounding makes the picked root or segment side ambiguous, which is
//!   what the `EdgeSkip` band is for.
//! * Anti-aliased contour coverage (AA is switched off and assertions sit well
//!   inside contours), non-Linear color spaces, blend modes other than
//!   SourceOver, and spread folding several periods away from the gradient.
//! * Tiler paths for pixmaps wider than 8192 px, which cannot occur on these
//!   small canvases.

use tiny_skia::*;

/// Max per-channel byte difference allowed on a numerically stable pixel.
const BYTE_TOL: i64 = 1;
/// A largest algebraic root in (-EDGE_T_MAX, 0] marks the unstable t=0 edge
/// band: neither side can be forced onto a deterministic result in f32.
const EDGE_T_MAX: f64 = 0.5;

#[derive(Clone, Copy, Debug)]
struct Affine {
    sx: f64, ky: f64, kx: f64, sy: f64, tx: f64, ty: f64,
}
impl Affine {
    fn id() -> Self { Affine { sx: 1., ky: 0., kx: 0., sy: 1., tx: 0., ty: 0. } }
    fn map(&self, x: f64, y: f64) -> (f64, f64) {
        (x * self.sx + y * self.kx + self.tx, x * self.ky + y * self.sy + self.ty)
    }
    /// Independent 2x3 affine inversion via the 2x2 determinant and adjugate.
    fn invert(&self) -> Self {
        let det = self.sx * self.sy - self.ky * self.kx;
        let inv = 1.0 / det;
        Affine {
            sx: self.sy * inv,
            ky: -self.ky * inv,
            kx: -self.kx * inv,
            sy: self.sx * inv,
            tx: (self.kx * self.ty - self.sy * self.tx) * inv,
            ty: (self.ky * self.tx - self.sx * self.ty) * inv,
        }
    }
}

#[derive(Clone, Copy, Debug)]
struct Stop {
    pos: f64,
    rgba: [f64; 4], // unpremultiplied, 0..1
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Spread { Pad, Repeat, Reflect }

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Geometry { Valid, EdgeSkip, Invalid }

struct Case {
    name: &'static str,
    w: u32,
    h: u32,
    c0: (f64, f64),
    r0: f64,
    c1: (f64, f64),
    r1: f64,
    stops: Vec<Stop>,
    spread: Spread,
    shader_ts: Affine,
    path_ts: Affine,
    /// Filled rectangle in path space: x0,y0,x1,y1. Everything outside it is a
    /// "contour outside" assertion and must keep the backdrop unchanged.
    path_rect: (f64, f64, f64, f64),
    backdrop: [u8; 4],
}

fn spread_t(t: f64, spread: Spread) -> f64 {
    match spread {
        Spread::Pad => t.clamp(0., 1.),
        Spread::Repeat => {
            let v = t - t.floor();
            if v >= 1. { 0. } else { v }
        }
        // Mirrors the pipeline ReflectX1 folding on a unit gradient domain.
        Spread::Reflect => {
            let v = ((t - 1.) - 2. * (((t - 1.) * 0.5).floor()) - 1.).abs();
            v.clamp(0., 1.)
        }
    }
}

fn stop_color(t: f64, stops: &[Stop]) -> [f64; 4] {
    if t <= stops[0].pos {
        return stops[0].rgba;
    }
    for w in stops.windows(2) {
        let (lo, hi) = (w[0], w[1]);
        if t <= hi.pos {
            // Repeated positions form a hard jump: at the boundary the right
            // stop wins, everywhere else the segment is linear.
            if hi.pos <= lo.pos {
                return hi.rgba;
            }
            let f = (t - lo.pos) / (hi.pos - lo.pos);
            let mut out = [0.; 4];
            for i in 0..4 {
                out[i] = lo.rgba[i] + (hi.rgba[i] - lo.rgba[i]) * f;
            }
            return out;
        }
    }
    stops[stops.len() - 1].rgba
}

fn round_byte(v: f64) -> u8 {
    (v.clamp(0., 1.) * 255.).round() as u8
}

/// Solves the two-circle geometry in gradient space.
///
/// For center C0/r0, C1/r1 and a point P, points on the interpolated cone are
/// `C(t) = C0 + t*(C1-C0)`, `r(t) = r0 + t*(r1-r0)`, and P must satisfy
/// `|P - C(t)|^2 = r(t)^2`, which expands to the scalar quadratic
/// `A*t^2 + B*t + C = 0` with
/// `A = d^2 - dr^2`, `B = -2*(u*d + r0*dr)`, `C = u^2 + v^2 - r0^2`
/// in a frame whose u axis runs from C0 to C1. The concentric case uses the
/// direct radius ratio; every off-center configuration, including the
/// equal-radius strip, is solved by this single quadratic.
fn classify(
    gx: f64,
    gy: f64,
    c: &Case,
) -> (Geometry, f64 /*raw t when valid*/, f64 /*diagnostic: disc or max root*/) {
    let d = (c.c1.0 - c.c0.0).hypot(c.c1.1 - c.c0.1);
    if d < 1e-9 {
        // Concentric circles: t follows the radius ratio directly.
        let dist = (gx - c.c0.0).hypot(gy - c.c0.1);
        let t = (dist - c.r0) / (c.r1 - c.r0);
        return (Geometry::Valid, t, t);
    }

    let dx = (c.c1.0 - c.c0.0) / d;
    let dy = (c.c1.1 - c.c0.1) / d;
    let vx = gx - c.c0.0;
    let vy = gy - c.c0.1;
    let u = vx * dx + vy * dy;
    let v = -vx * dy + vy * dx;
    let dr = c.r1 - c.r0;
    // The same quadratic covers the equal-radius strip (dr = 0, A = d^2): its
    // +sqrt root is exactly the pipeline's strip branch, and a negative
    // radicand (r0^2 - v^2 < 0) shows up as a negative discriminant below.
    let a = d * d - dr * dr;
    let b = -2.0 * (u * d + c.r0 * dr);
    let cc = u * u + v * v - c.r0 * c.r0;
    let disc = b * b - 4. * a * cc;
    if disc < 0. {
        return (Geometry::Invalid, f64::NAN, disc);
    }
    let s = disc.sqrt();
    let t1 = (-b - s) / (2. * a);
    let t2 = (-b + s) / (2. * a);
    let max_root = t1.max(t2);
    if max_root > 0. {
        // The pipeline keeps the largest positive cone intersection.
        (Geometry::Valid, max_root, disc)
    } else if max_root > -EDGE_T_MAX {
        // Both roots are negative but graze t=0: f32 sign handling at the cone
        // edge is not deterministic enough to pin down, so no byte assertion.
        (Geometry::EdgeSkip, f64::NAN, max_root)
    } else {
        (Geometry::Invalid, f64::NAN, max_root)
    }
}

/// Full independent expectation for one pixel, in premultiplied RGBA bytes.
/// `inside_contour` reports whether the pixel center is inside the filled path.
fn reference_pixel(
    x: u32,
    y: u32,
    mask: u8,
    c: &Case,
) -> ([u8; 4], Geometry, f64, bool) {
    let backdrop = c.backdrop;
    let device = (x as f64 + 0.5, y as f64 + 0.5);

    // Device -> path space, then path space -> gradient space. The two
    // transforms are deliberately both present and never cancel.
    let path_pt = c.path_ts.invert().map(device.0, device.1);
    let inside = path_pt.0 >= c.path_rect.0
        && path_pt.0 <= c.path_rect.2
        && path_pt.1 >= c.path_rect.1
        && path_pt.1 <= c.path_rect.3;
    if !inside {
        return (backdrop, Geometry::Invalid, f64::NAN, false);
    }

    let (gx, gy) = c.shader_ts.invert().map(path_pt.0, path_pt.1);
    let (geo, raw_t, diag) = classify(gx, gy, c);
    match geo {
        Geometry::Valid => {}
        // No drawable source: the mask clears it and the backdrop survives.
        Geometry::Invalid | Geometry::EdgeSkip => {
            return (backdrop, geo, diag, inside);
        }
    }

    let t = spread_t(raw_t, c.spread);
    let col = stop_color(t, &c.stops);
    let alpha = col[3];

    // Premultiply, then apply the 8-bit coverage mask (MaskU8 scales every
    // premultiplied channel), then SourceOver the opaque colored backdrop.
    let coverage = mask as f64 / 255.;
    if coverage == 0. {
        return (backdrop, Geometry::Valid, raw_t, inside);
    }
    let sr = col[0] * alpha * coverage;
    let sg = col[1] * alpha * coverage;
    let sb = col[2] * alpha * coverage;
    let sa = alpha * coverage;

    let dst = [
        backdrop[0] as f64 / 255.,
        backdrop[1] as f64 / 255.,
        backdrop[2] as f64 / 255.,
        backdrop[3] as f64 / 255.,
    ];
    let out = [
        sr + dst[0] * (1. - sa),
        sg + dst[1] * (1. - sa),
        sb + dst[2] * (1. - sa),
        sa + dst[3] * (1. - sa),
    ];
    (
        [round_byte(out[0]), round_byte(out[1]), round_byte(out[2]), round_byte(out[3])],
        Geometry::Valid,
        raw_t,
        inside,
    )
}

fn make_stops() -> Vec<Stop> {
    // Asymmetric positions, varying alpha, and a repeated 0.35 position that
    // creates a hard green->blue jump. Dummy 0/1 brackets are implicit.
    let raw: &[(f32, (u8, u8, u8, u8))] = &[
        (0.00, (255, 0, 0, 200)),
        (0.35, (0, 255, 0, 120)),
        (0.35, (0, 0, 255, 255)),
        (0.80, (255, 255, 0, 90)),
        (1.00, (0, 255, 255, 255)),
    ];
    raw.iter()
        .map(|(p, (r, g, b, a))| Stop {
            pos: *p as f64,
            rgba: [*r as f64 / 255., *g as f64 / 255., *b as f64 / 255., *a as f64 / 255.],
        })
        .collect()
}

fn mask_for(width: u32, height: u32) -> Mask {
    let mut mask = Mask::new(width, height).unwrap();
    for y in 0..height {
        for x in 0..width {
            let v = if x < 2 {
                0 // explicit zero coverage
            } else if x < width - 4 {
                64 // partial coverage
            } else if x < width - 1 {
                255 // full coverage
            } else {
                128 // second partial coverage, reaches the SIMD tail
            };
            mask.data_mut()[(y * width + x) as usize] = v;
        }
    }
    mask
}

fn render(c: &Case, force_hq: bool, mask: &Mask) -> Pixmap {
    let mut pm = Pixmap::new(c.w, c.h).unwrap();
    pm.fill(Color::from_rgba8(
        c.backdrop[0], c.backdrop[1], c.backdrop[2], c.backdrop[3],
    ));

    let mode = match c.spread {
        Spread::Pad => SpreadMode::Pad,
        Spread::Repeat => SpreadMode::Repeat,
        Spread::Reflect => SpreadMode::Reflect,
    };
    let stops: Vec<GradientStop> = c
        .stops
        .iter()
        .map(|s| {
            GradientStop::new(
                s.pos as f32,
                Color::from_rgba8(
                    round_byte(s.rgba[0]),
                    round_byte(s.rgba[1]),
                    round_byte(s.rgba[2]),
                    round_byte(s.rgba[3]),
                ),
            )
        })
        .collect();
    let gts = Transform::from_row(
        c.shader_ts.sx as f32, c.shader_ts.ky as f32, c.shader_ts.kx as f32,
        c.shader_ts.sy as f32, c.shader_ts.tx as f32, c.shader_ts.ty as f32,
    );
    let shader = RadialGradient::new(
        Point::from_xy(c.c0.0 as f32, c.c0.1 as f32),
        c.r0 as f32,
        Point::from_xy(c.c1.0 as f32, c.c1.1 as f32),
        c.r1 as f32,
        stops,
        mode,
        gts,
    )
    .unwrap();

    let mut paint = Paint::default();
    paint.anti_alias = false;
    paint.force_hq_pipeline = force_hq;
    paint.shader = shader;

    let path = PathBuilder::from_rect(
        Rect::from_ltrb(
            c.path_rect.0 as f32,
            c.path_rect.1 as f32,
            c.path_rect.2 as f32,
            c.path_rect.3 as f32,
        )
        .unwrap(),
    );
    let pts = Transform::from_row(
        c.path_ts.sx as f32, c.path_ts.ky as f32, c.path_ts.kx as f32,
        c.path_ts.sy as f32, c.path_ts.tx as f32, c.path_ts.ty as f32,
    );
    pm.fill_path(&path, &paint, FillRule::Winding, pts, Some(mask));
    pm
}

#[derive(Default)]
struct Counts {
    valid: u32,
    invalid: u32,
    edge_skip: u32,
    outside: u32,
    tail: u32,
    out_of_range: u32,
    failures: u32,
}

fn run_case(c: &Case, force_hq: bool, failures: &mut Vec<String>) -> Counts {
    let mut counts = Counts::default();
    let mask = mask_for(c.w, c.h);
    let pm = render(c, force_hq, &mask);
    let tail_start = (c.w / 8) * 8; // highp SIMD stage width is 8; lowp is 16.

    for y in 0..c.h {
        for x in 0..c.w {
            let m = mask.data()[(y * c.w + x) as usize];
            let (expected, geo, raw_t, inside) =
                reference_pixel(x, y, m, c);
            let pixel = pm.pixel(x, y).unwrap();
            let actual = [pixel.red(), pixel.green(), pixel.blue(), pixel.alpha()];

            if x >= tail_start {
                counts.tail += 1;
            }
            if !inside {
                counts.outside += 1;
            }
            match geo {
                Geometry::Valid => {
                    counts.valid += 1;
                    if raw_t.is_finite() && (raw_t < 0. || raw_t > 1.) {
                        counts.out_of_range += 1;
                    }
                    let diff = actual
                        .iter()
                        .zip(expected)
                        .map(|(a, b)| (*a as i64 - b as i64).abs())
                        .max()
                        .unwrap();
                    if diff > BYTE_TOL {
                        counts.failures += 1;
                        failures.push(format!(
                            "ASSERT_FAIL case={} hq={} pixel=({},{}) mask={} geometry=Valid raw_t={:.6} \
                             expected={:?} actual={:?}",
                            c.name, force_hq, x, y, m, raw_t, expected, actual
                        ));
                    }
                }
                Geometry::Invalid => {
                    counts.invalid += 1;
                    // Transparent source: the backdrop must be exactly preserved.
                    if actual != expected {
                        counts.failures += 1;
                        failures.push(format!(
                            "ASSERT_FAIL case={} hq={} pixel=({},{}) mask={} geometry=Invalid \
                             expected_backdrop={:?} actual={:?}",
                            c.name, force_hq, x, y, m, expected, actual
                        ));
                    }
                }
                Geometry::EdgeSkip => counts.edge_skip += 1,
            }
        }
    }
    counts
}

fn cases() -> Vec<Case> {
    let stops = make_stops();
    // Non-cancelling pair: shader transform skews/scales gradient space while
    // the path transform independently remaps contour coordinates.
    let gts = Affine { sx: 1.25, ky: 0.15, kx: -0.2, sy: 0.9, tx: 1.3, ty: -0.7 };
    let pts = Affine { sx: 0.9, ky: 0.0, kx: 0.1, sy: 1.1, tx: 0.6, ty: -0.4 };
    let backdrop = [40u8, 80, 120, 255];
    // Filled contour leaves a one-pixel border on every side so that pixels
    // outside both the contour and the mask are provably left untouched.
    let rect13 = (1.5, 1.5, 11.5, 9.5);

    vec![
        // Mutation control: concentric, both radii non-zero, every transform
        // identity. Neither planted mutation can move these pixels.
        Case {
            name: "control_concentric_identity",
            w: 9, h: 9,
            c0: (4.5, 4.5), r0: 2.0, c1: (4.5, 4.5), r1: 8.0,
            stops: stops.clone(), spread: Spread::Pad,
            shader_ts: Affine::id(), path_ts: Affine::id(),
            path_rect: (0., 0., 9., 9.),
            backdrop,
        },
        // Concentric with both radii non-zero; Pad sampling past t=1.
        Case {
            name: "concentric_pad_13x11",
            w: 13, h: 11,
            c0: (6., 5.), r0: 2.0, c1: (6., 5.), r1: 8.0,
            stops: stops.clone(), spread: Spread::Pad,
            shader_ts: gts, path_ts: pts,
            path_rect: rect13,
            backdrop,
        },
        // Shrinking radii with Repeat folding outside 0..1.
        Case {
            name: "shrinking_repeat_13x11",
            w: 13, h: 11,
            c0: (6., 5.), r0: 8.0, c1: (6., 5.), r1: 2.0,
            stops: stops.clone(), spread: Spread::Repeat,
            shader_ts: gts, path_ts: pts,
            path_rect: rect13,
            backdrop,
        },
        // Distinct centers, well-behaved focal cone with an invalid shadow.
        Case {
            name: "focal_pad_13x11",
            w: 13, h: 11,
            c0: (3., 5.5), r0: 1.0, c1: (9., 5.5), r1: 6.0,
            stops: stops.clone(), spread: Spread::Pad,
            shader_ts: gts, path_ts: pts,
            path_rect: rect13,
            backdrop,
        },
        // Equal-radius strip: negative-radicand points must stay transparent.
        Case {
            name: "strip_pad_13x11",
            w: 13, h: 11,
            c0: (3., 5.5), r0: 3.0, c1: (9., 5.5), r1: 3.0,
            stops: stops.clone(), spread: Spread::Pad,
            shader_ts: gts, path_ts: pts,
            path_rect: rect13,
            backdrop,
        },
        // Focal point outside the end circle plus Reflect folding.
        Case {
            name: "focus_outside_reflect_13x11",
            w: 13, h: 11,
            c0: (3., 5.5), r0: 5.0, c1: (8., 5.5), r1: 2.5,
            stops: stops.clone(), spread: Spread::Reflect,
            shader_ts: gts, path_ts: pts,
            path_rect: rect13,
            backdrop,
        },
    ]
}

fn main() {
    let all_cases = cases();
    let mut failures: Vec<String> = Vec::new();
    let mut executed = 0u32;
    let mut totals = Counts::default();
    let mut per_mode_oor = [0u32; 3];

    for case in &all_cases {
        for force_hq in [false, true] {
            executed += 1;
            let counts = run_case(case, force_hq, &mut failures);
            println!(
                "CASE_DONE name={} hq={} valid={} invalid={} edge_skip={} outside={} \
                 tail_pixels={} out_of_range_t={} failures={}",
                case.name,
                force_hq,
                counts.valid,
                counts.invalid,
                counts.edge_skip,
                counts.outside,
                counts.tail,
                counts.out_of_range,
                counts.failures
            );
            totals.valid += counts.valid;
            totals.invalid += counts.invalid;
            totals.edge_skip += counts.edge_skip;
            totals.outside += counts.outside;
            totals.tail += counts.tail;
            totals.out_of_range += counts.out_of_range;
            totals.failures += counts.failures;
            match case.spread {
                Spread::Pad => per_mode_oor[0] += counts.out_of_range,
                Spread::Repeat => per_mode_oor[1] += counts.out_of_range,
                Spread::Reflect => per_mode_oor[2] += counts.out_of_range,
            }
        }
    }

    println!(
        "RADIAL_CASES_EXECUTED n={} ({} cases x default + force_hq pipelines)",
        executed,
        all_cases.len()
    );
    println!(
        "RADIAL_GEOMETRY valid={} invalid={} edge_skip={} outside_contour={} simd_tail_pixels={}",
        totals.valid, totals.invalid, totals.edge_skip, totals.outside, totals.tail
    );
    println!(
        "RADIAL_OUT_OF_RANGE pad={} repeat={} reflect={}",
        per_mode_oor[0], per_mode_oor[1], per_mode_oor[2]
    );

    // Material invariants: every spread mode must actually sample outside 0..1,
    // every canvas contributes SIMD-tail pixels, and invalid geometry exists.
    if per_mode_oor.iter().any(|n| *n == 0) {
        failures.push(
            "ASSERT_FAIL material: each spread mode needs out-of-range samples".to_string(),
        );
    }
    if totals.tail == 0 {
        failures.push("ASSERT_FAIL material: no SIMD tail pixels were compared".to_string());
    }
    if totals.invalid == 0 {
        failures.push("ASSERT_FAIL material: no invalid two-circle pixels were exercised".to_string());
    }
    if totals.outside == 0 {
        failures.push("ASSERT_FAIL material: no outside-contour pixels were checked".to_string());
    }

    if failures.is_empty() {
        println!(
            "RADIAL_CONTRACT_PASS byte_tol={} compared_pixels={}",
            BYTE_TOL,
            totals.valid
        );
    } else {
        for f in &failures {
            println!("{}", f);
        }
        println!(
            "RADIAL_CONTRACT_FAIL failures={} byte_tol={}",
            failures.len(),
            BYTE_TOL
        );
        std::process::exit(1);
    }
}
