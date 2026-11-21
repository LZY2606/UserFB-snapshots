//! Ideal planar mirrors and deterministic, specular light transport.
//!
//! This module adds a small number of finite, perfectly flat, double-sided mirror
//! panels to a scene. Unlike rough materials, which can only find specular paths by
//! randomly sampling directions, [`MirrorTransport`] enumerates every ordered sequence
//! of mirror reflections and reconstructs the corresponding folded polyline using the
//! virtual-image (unfolding) construction. The same solved paths are used for the actual
//! shading contribution, so there is no separate diagnostic solver.
//!
//! # Geometry
//!
//! A [`MirrorPanel`] is the finite parallelogram
//! `center + a * edge_u + b * edge_v` with `a, b in [-1, 1]`, where `edge_u` and
//! `edge_v` are orthogonal, nonzero half-edge vectors. Panels are ideal reflectors on
//! both sides and carry a per-channel RGB reflectance in `[0, 1]`.
//!
//! For a point light `L`, a receiver `P` and an ordered mirror sequence
//! `[m1, m2, ..., mk]`, the light is reflected across the panel planes in sequence to
//! build virtual images `L0 = L`, `Lj = reflect(Lj-1, mj)`. A straight line from the
//! receiver to the final image `Lk` is then folded back through the planes, producing
//! the world-space reflection vertices. Reflection at every vertex automatically obeys
//! the law of reflection, so no random direction sampling is involved.
//!
//! # Degenerate configurations
//!
//! The following configurations produce no contribution and are silently skipped (they
//! never cause panics):
//!
//! - a segment whose unfolded line is parallel to, or coplanar with, a mirror plane;
//! - a zero-length segment, which includes a light or receiver lying on a mirror plane
//!   and a reflection vertex collapsing onto an endpoint;
//! - a reflection vertex that falls outside the finite aperture of its panel (the
//!   aperture boundary itself counts as interior);
//! - a segment occluded by an ordinary `Scene` object, or by a panel that is not one of
//!   the segment's two endpoints (mirrors are never treated as transparent markers).
//!
//! Only self-intersections at the two endpoints of a segment are ignored. An object or
//! mirror crossing the segment anywhere in its interior blocks it, and objects behind
//! the point light do not block anything because only the finite segment is tested.
//!
//! # Finite-segment tolerances
//!
//! All geometric predicates use fixed absolute tolerances in world units:
//!
//! - [`MIRROR_EPSILON`] (1e-10): minimum positive segment length, endpoint exclusion
//!   distance for occlusion tests, and absolute slack when testing whether a reflection
//!   vertex lies inside a finite aperture;
//! - [`MIRROR_PARALLEL_EPSILON`] (1e-12): relative threshold below which a line and a
//!   plane are treated as parallel;
//! - [`MIRROR_TIME_EPSILON`] (1e-12): dimensionless slack on the unfolded line parameter,
//!   which must lie strictly in `(0, 1)`.
//!
//! # Contribution
//!
//! The point light intensity is divided by the square of the *total* polyline length
//! (never attenuated per segment), multiplied by the product of the per-reflection RGB
//! reflectances, and evaluated with the receiver material's existing BSDF and forward
//! cosine using the direction of the segment leaving the receiver. Flat reflection
//! introduces no additional area Jacobian.

use crate::color::Color;
use crate::scene::Scene;
use crate::shape::{HitRecord, Ray};

/// Absolute world-space tolerance for segment lengths, aperture tests and occlusion.
pub const MIRROR_EPSILON: f64 = 1e-10;

/// Relative threshold for treating an unfolded line as parallel to a mirror plane.
pub const MIRROR_PARALLEL_EPSILON: f64 = 1e-12;

/// Dimensionless tolerance on the unfolded line parameter, which must stay in `(0, 1)`.
pub const MIRROR_TIME_EPSILON: f64 = 1e-12;

/// Relative tolerance for the orthogonality check on the two half-edge vectors.
const MIRROR_ORTHOGONAL_EPSILON: f64 = 1e-10;

/// Maximum number of mirror panels accepted by [`MirrorTransport::new`].
pub const MAX_MIRROR_PANELS: usize = 8;

/// Maximum value accepted for the per-connection reflection budget.
pub const MAX_MIRROR_REFLECTIONS: usize = 4;

/// A finite, double-sided, ideal planar mirror.
///
/// The panel covers `center + a * edge_u + b * edge_v` with `a, b in [-1, 1]`.
/// `edge_u` and `edge_v` must be orthogonal and nonzero, and every channel of
/// `reflectance` must lie in `[0, 1]`. These invariants are checked by
/// [`MirrorTransport::new`].
#[derive(Copy, Clone, Debug)]
pub struct MirrorPanel {
    /// Center of the parallelogram in world space.
    pub center: glm::DVec3,

    /// Orthogonal nonzero half-edge vector spanning the first aperture direction.
    pub edge_u: glm::DVec3,

    /// Orthogonal nonzero half-edge vector spanning the second aperture direction.
    pub edge_v: glm::DVec3,

    /// Ideal RGB reflectance of the panel, with each channel in `[0, 1]`.
    pub reflectance: Color,
}

impl MirrorPanel {
    /// Unit normal of the panel, oriented along `edge_u x edge_v`.
    ///
    /// Reflection is double-sided, so the sign of this normal does not change results.
    pub fn normal(&self) -> glm::DVec3 {
        self.edge_u.cross(&self.edge_v).normalize()
    }

    /// Reflect a point across the panel's infinite supporting plane.
    fn reflect_point(&self, point: &glm::DVec3) -> glm::DVec3 {
        let normal = self.normal();
        let distance = (point - self.center).dot(&normal);
        point - 2.0 * distance * normal
    }

    /// Whether a point on the panel's supporting plane lies inside its finite aperture.
    ///
    /// The aperture boundary is included as interior, with [`MIRROR_EPSILON`] slack.
    fn contains(&self, point: glm::DVec3) -> bool {
        let delta = point - self.center;
        let coord_u = delta.dot(&self.edge_u) / self.edge_u.magnitude_squared();
        let coord_v = delta.dot(&self.edge_v) / self.edge_v.magnitude_squared();
        let slack_u = MIRROR_EPSILON / self.edge_u.magnitude();
        let slack_v = MIRROR_EPSILON / self.edge_v.magnitude();
        coord_u.abs() <= 1.0 + slack_u && coord_v.abs() <= 1.0 + slack_v
    }

    /// Intersect a ray with the finite panel, returning the ray time of the hit.
    pub(crate) fn intersect_ray(&self, ray: &Ray, t_min: f64) -> Option<f64> {
        let normal = self.normal();
        let denom = normal.dot(&ray.dir);
        if denom.abs() < MIRROR_PARALLEL_EPSILON {
            // Parallel ray and mirror plane
            return None;
        }
        let time = (self.center - ray.origin).dot(&normal) / denom;
        if time < t_min {
            return None;
        }
        let point = ray.at(time);
        if self.contains(point) {
            Some(time)
        } else {
            None
        }
    }
}

/// A solved, geometrically valid specular path between a point light and a receiver.
///
/// Every `MirrorPath` returned by [`MirrorTransport::connect`] is unoccluded and is the
/// exact object used by the `Renderer` when accumulating light, so observing these
/// values observes the paths that genuinely participate in shading.
#[derive(Clone, Debug)]
pub struct MirrorPath {
    /// Ordered panel indices struck by the path, starting on the light side.
    pub panels: Vec<usize>,

    /// World-space reflection vertices (fold points), in the same order as `panels`.
    pub points: Vec<glm::DVec3>,

    /// Total length of the folded polyline, summed over all of its segments.
    pub total_length: f64,

    /// Product of the RGB reflectances of every panel in `panels`.
    pub reflectance: Color,
}

/// Read-only collection of finite mirror panels plus the per-connection reflection budget.
///
/// Construct once with [`MirrorTransport::new`] and share it between renderers. The
/// configuration is never mutated after construction and does not modify the `Scene`
/// or any existing `Material`.
#[derive(Debug)]
pub struct MirrorTransport {
    panels: Vec<MirrorPanel>,
    max_reflections: usize,
}

impl MirrorTransport {
    /// Construct a mirror transport from finite panels and a reflection budget.
    ///
    /// At most [`MAX_MIRROR_PANELS`] panels are accepted, `max_reflections` must be in
    /// `0..=`[`MAX_MIRROR_REFLECTIONS`], every panel must have finite, nonzero,
    /// orthogonal half-edges, a finite center and a reflectance with all channels in
    /// `[0, 1]`. Panel intersections and overlaps are the caller's responsibility.
    pub fn new(panels: Vec<MirrorPanel>, max_reflections: usize) -> Result<Self, String> {
        if panels.len() > MAX_MIRROR_PANELS {
            return Err(format!(
                "expected at most {} mirror panels, found {}",
                MAX_MIRROR_PANELS,
                panels.len()
            ));
        }
        if max_reflections > MAX_MIRROR_REFLECTIONS {
            return Err(format!(
                "max_reflections must be in 0..={}, found {}",
                MAX_MIRROR_REFLECTIONS,
                max_reflections
            ));
        }
        for (index, panel) in panels.iter().enumerate() {
            let finite = |v: glm::DVec3| v.x.is_finite() && v.y.is_finite() && v.z.is_finite();
            if !finite(panel.center) {
                return Err(format!("panel {} has a non-finite center", index));
            }
            if !finite(panel.edge_u) || panel.edge_u.magnitude_squared() == 0.0 {
                return Err(format!("panel {} has a zero or non-finite edge_u", index));
            }
            if !finite(panel.edge_v) || panel.edge_v.magnitude_squared() == 0.0 {
                return Err(format!("panel {} has a zero or non-finite edge_v", index));
            }
            let orthogonality =
                panel.edge_u.dot(&panel.edge_v).abs() / (panel.edge_u.magnitude() * panel.edge_v.magnitude());
            if orthogonality > MIRROR_ORTHOGONAL_EPSILON {
                return Err(format!(
                    "panel {} has non-orthogonal half-edges (cosine {})",
                    index, orthogonality
                ));
            }
            let reflectance = panel.reflectance;
            if !finite(reflectance)
                || reflectance.x < 0.0
                || reflectance.y < 0.0
                || reflectance.z < 0.0
                || reflectance.x > 1.0
                || reflectance.y > 1.0
                || reflectance.z > 1.0
            {
                return Err(format!(
                    "panel {} reflectance channels must all be finite and in [0, 1]",
                    index
                ));
            }
        }
        Ok(Self {
            panels,
            max_reflections,
        })
    }

    /// The configured mirror panels, in index order.
    pub fn panels(&self) -> &[MirrorPanel] {
        &self.panels
    }

    /// The maximum number of reflections allowed in a single connection.
    pub fn max_reflections(&self) -> usize {
        self.max_reflections
    }

    /// Solve every valid specular connection from a point light to a receiver point.
    ///
    /// All ordered panel sequences of length 1..=`max_depth` (also capped by the
    /// configured reflection budget) are unfolded with virtual images. Consecutive
    /// repetitions of the same panel are skipped, since they collapse onto a shorter
    /// sequence, while non-adjacent repetitions such as `A -> B -> A` are retained.
    ///
    /// The returned paths all have positive-length segments, reflection vertices inside
    /// finite panel apertures and no occlusion by scene objects or unrelated panels.
    /// This is the single solver used both for rendering and for observation.
    pub fn connect(
        &self,
        scene: &Scene,
        light: &glm::DVec3,
        receiver: &glm::DVec3,
        max_depth: usize,
    ) -> Vec<MirrorPath> {
        let depth_cap = max_depth.min(self.max_reflections);
        if depth_cap == 0 || self.panels.is_empty() {
            return Vec::new();
        }
        let mut paths = Vec::new();
        let mut sequence = Vec::new();
        self.enumerate(&mut sequence, depth_cap, &mut |sequence| {
            if let Some(path) = self.try_path(scene, light, receiver, sequence) {
                paths.push(path);
            }
        });
        paths
    }

    /// Depth-first enumeration of ordered panel sequences without consecutive repeats.
    fn enumerate(&self, sequence: &mut Vec<usize>, depth_cap: usize, emit: &mut impl FnMut(&[usize])) {
        if sequence.len() >= depth_cap {
            return;
        }
        for index in 0..self.panels.len() {
            if sequence.last().copied() == Some(index) {
                // Hitting the same panel twice in a row is degenerate and can be omitted.
                continue;
            }
            sequence.push(index);
            emit(sequence);
            self.enumerate(sequence, depth_cap, emit);
            sequence.pop();
        }
    }

    /// Reconstruct one folded polyline for a fixed ordered panel sequence.
    fn try_path(
        &self,
        scene: &Scene,
        light: &glm::DVec3,
        receiver: &glm::DVec3,
        sequence: &[usize],
    ) -> Option<MirrorPath> {
        let depth = sequence.len();

        // Virtual images of the light, reflected across m1, m2, ... in order.
        let mut images = Vec::with_capacity(depth + 1);
        images.push(*light);
        for &panel_index in sequence {
            let panel = &self.panels[panel_index];
            images.push(panel.reflect_point(images.last().unwrap()));
        }

        // Fold the straight receiver-to-image line back through the panel planes.
        let mut vertices = vec![glm::zero::<glm::DVec3>(); depth];
        let mut origin = *receiver;
        let mut target = images[depth];
        for step in (0..depth).rev() {
            let panel = &self.panels[sequence[step]];
            let point = line_panel_intersect(origin, target, panel)?;
            vertices[step] = point;
            origin = point;
            target = images[step];
        }

        // Polyline: light -> r1 -> ... -> rk -> receiver. Every segment must have
        // positive length and survive full occlusion testing.
        let mut points = Vec::with_capacity(depth + 2);
        points.push(*light);
        points.extend(vertices.iter().copied());
        points.push(*receiver);

        let mut total_length = 0.0;
        for segment in 0..=depth {
            let start = points[segment];
            let end = points[segment + 1];
            let length = (end - start).magnitude();
            if length <= MIRROR_EPSILON {
                // Zero-length segment: light, vertex or receiver collapsed.
                return None;
            }
            if self.segment_blocked(scene, start, end, length, sequence) {
                return None;
            }
            total_length += length;
        }

        let mut reflectance = glm::vec3(1.0, 1.0, 1.0);
        for &panel_index in sequence {
            reflectance = reflectance.component_mul(&self.panels[panel_index].reflectance);
        }
        Some(MirrorPath {
            panels: sequence.to_vec(),
            points: vertices,
            total_length,
            reflectance,
        })
    }

    /// Test whether the finite segment `start -> end` is blocked by scene geometry.
    ///
    /// Ordinary scene objects are tested first, then every panel. Hits within
    /// [`MIRROR_EPSILON`] of either endpoint are self-intersections and are ignored;
    /// anything in the interior, including a panel from the configured sequence that the
    /// ray reaches ahead of schedule, blocks the segment.
    fn segment_blocked(
        &self,
        scene: &Scene,
        start: glm::DVec3,
        end: glm::DVec3,
        length: f64,
        _sequence: &[usize],
    ) -> bool {
        let ray = Ray {
            origin: start,
            dir: (end - start) / length,
        };
        let cutoff = length - MIRROR_EPSILON;

        let mut record = HitRecord::new();
        for object in &scene.objects {
            if object.shape.intersect(&ray, MIRROR_EPSILON, &mut record) && record.time < cutoff {
                return true;
            }
        }
        for panel in &self.panels {
            if let Some(time) = panel.intersect_ray(&ray, MIRROR_EPSILON) {
                if time < cutoff {
                    return true;
                }
            }
        }
        false
    }
}

/// Intersect the finite line segment `origin -> target` with a panel plane.
///
/// Returns the reflection vertex only when the line is not parallel, the plane crossing
/// lies strictly between the two endpoints and the vertex lands inside the aperture.
fn line_panel_intersect(
    origin: glm::DVec3,
    target: glm::DVec3,
    panel: &MirrorPanel,
) -> Option<glm::DVec3> {
    let displacement = target - origin;
    let normal = panel.normal();
    let denom = normal.dot(&displacement);
    if denom.abs() <= MIRROR_PARALLEL_EPSILON * displacement.magnitude() {
        // Parallel or coplanar unfolded line: no unique reflection vertex.
        return None;
    }
    let time = (panel.center - origin).dot(&normal) / denom;
    if time <= MIRROR_TIME_EPSILON || time >= 1.0 - MIRROR_TIME_EPSILON {
        // Crossing behind the segment or at an endpoint (zero-length fold).
        return None;
    }
    let point = origin + time * displacement;
    if panel.contains(point) {
        Some(point)
    } else {
        None
    }
}
