//! Ideal, finite planar mirror panels and deterministic point-light transport.
//!
//! A [`MirrorPanel`] is a finite rectangle described by its center and two
//! orthogonal, nonzero half-edge vectors: every point on the panel is of the
//! form `center + a * edge_u + b * edge_v`, with `a, b in [-1, 1]`. Panels are
//! two-sided ideal mirrors, so light reflects specularly off either face and
//! the panel normal used for reflection always faces the incoming ray.
//!
//! Point-light connections are found analytically with the method of mirror
//! images rather than by random sampling. For an ordered panel sequence
//! `m1, m2, ..., mk` visited by light traveling from the point light to the
//! receiver, the light is successively reflected across each panel plane. The
//! straight segment from the receiver to the final virtual image is then
//! unfolded back into a polyline whose vertices satisfy the law of reflection
//! exactly. Every segment is tested for positive length, in-aperture fold
//! points and occlusion before the sequence is accepted.
//!
//! Degenerate configurations never contribute:
//!
//! - a segment parallel to (or coplanar with) a panel plane has no unique
//!   intersection and is rejected;
//! - a segment whose length is not strictly positive is rejected;
//! - a fold point outside the finite rectangle is rejected.
//!
//! Finite-segment tolerances (all absolute, except where noted):
//!
//! - [`LENGTH_EPS`] is the minimum positive segment length and the open
//!   interval offset used when checking occlusion, so intersections exactly at
//!   a segment endpoint (the receiver, the light, or a fold point) are treated
//!   as self-intersections and ignored;
//! - [`APERTURE_EPS`] is the relative slack on the `[-1, 1]` aperture
//!   coordinates, so a fold point computed on the rectangle boundary (for
//!   example `a = 1.0000000001` from floating-point rounding) counts as inside;
//! - [`PARALLEL_EPS`] is the absolute cosine threshold below which a segment is
//!   considered parallel to a panel plane.

use crate::color::Color;
use crate::scene::Scene;
use crate::shape::{Ray, Shape};

/// Minimum strictly-positive length of every polyline segment.
pub const LENGTH_EPS: f64 = 1e-10;

/// Relative tolerance for finite-aperture containment; the boundary is inside.
pub const APERTURE_EPS: f64 = 1e-10;

/// Absolute cosine threshold for treating a segment as parallel to a panel.
pub const PARALLEL_EPS: f64 = 1e-12;

/// Maximum number of panels accepted by [`MirrorTransport::new`].
pub const MAX_PANELS: usize = 8;

/// Maximum number of reflections in a single point-light connection.
pub const MAX_REFLECTIONS: usize = 4;

/// A finite, two-sided ideal planar mirror.
///
/// The rectangle spans `center + a * edge_u + b * edge_v` for
/// `a, b in [-1, 1]`. `edge_u` and `edge_v` are half-edge vectors: they must be
/// nonzero, finite and mutually orthogonal. `reflectance` is an RGB multiplier
/// applied on every reflection, with every channel in `[0, 1]`.
#[derive(Copy, Clone, Debug)]
pub struct MirrorPanel {
    /// Center of the rectangular panel in world coordinates.
    pub center: glm::DVec3,

    /// Orthogonal nonzero half-edge vector spanning the first aperture axis.
    pub edge_u: glm::DVec3,

    /// Orthogonal nonzero half-edge vector spanning the second aperture axis.
    pub edge_v: glm::DVec3,

    /// RGB reflectance applied at each reflection; channels are in `[0, 1]`.
    pub reflectance: Color,
}

impl MirrorPanel {
    /// Unit normal of the panel plane, oriented by `edge_u × edge_v`.
    pub fn normal(&self) -> glm::DVec3 {
        self.edge_u.cross(&self.edge_v).normalize()
    }

    /// Reflect a point across the (infinite) panel plane.
    fn reflect_point(&self, point: &glm::DVec3) -> glm::DVec3 {
        let normal = self.normal();
        let distance = normal.dot(&(point - self.center));
        point - 2.0 * distance * normal
    }

    /// Intersect the finite panel with the segment from `from` to `to`,
    /// returning the world-space intersection point when it lies in the
    /// aperture (boundary included, see [`APERTURE_EPS`]).
    fn intersect_segment(&self, from: &glm::DVec3, to: &glm::DVec3) -> Option<glm::DVec3> {
        let direction = to - from;
        let normal = self.normal();
        let cosine = normal.dot(&direction);
        if cosine.abs() < PARALLEL_EPS {
            // Parallel or coplanar: no unique intersection.
            return None;
        }
        let time = normal.dot(&(self.center - from)) / cosine;
        if time <= LENGTH_EPS || time >= 1.0 - LENGTH_EPS {
            // The virtual image is on the wrong side, or the hit is an endpoint.
            return None;
        }
        let hit = from + time * direction;
        let offset = hit - self.center;
        let a = offset.dot(&self.edge_u) / self.edge_u.magnitude_squared();
        let b = offset.dot(&self.edge_v) / self.edge_v.magnitude_squared();
        let limit = 1.0 + APERTURE_EPS;
        if a.abs() <= limit && b.abs() <= limit {
            Some(hit)
        } else {
            None
        }
    }

    /// Intersect a ray with the finite panel, returning the ray time and the
    /// unit normal oriented toward the incoming ray.
    pub(crate) fn intersect_ray(&self, ray: &Ray) -> Option<(f64, glm::DVec3)> {
        let normal = self.normal();
        let cosine = normal.dot(&ray.dir);
        if cosine.abs() < PARALLEL_EPS {
            return None;
        }
        let time = normal.dot(&(self.center - ray.origin)) / cosine;
        if !time.is_finite() || time <= LENGTH_EPS {
            return None;
        }
        let hit = ray.at(time);
        let offset = hit - self.center;
        let a = offset.dot(&self.edge_u) / self.edge_u.magnitude_squared();
        let b = offset.dot(&self.edge_v) / self.edge_v.magnitude_squared();
        let limit = 1.0 + APERTURE_EPS;
        if a.abs() <= limit && b.abs() <= limit {
            // Two-sided mirror: face the incoming ray.
            let facing = if cosine < 0.0 { normal } else { -normal };
            Some((time, facing))
        } else {
            None
        }
    }
}

/// A validated, read-only collection of ideal mirror panels.
///
/// Constructed once and shared between renders. It never mutates the
/// [`Scene`] or any [`crate::Material`]; panels participate in eye paths and
/// occlusion directly, so they must not also be added to `Scene.objects`.
#[derive(Debug)]
pub struct MirrorTransport {
    panels: Vec<MirrorPanel>,
    max_reflections: usize,
}

impl MirrorTransport {
    /// Construct a mirror transport from panels and a per-connection reflection limit.
    ///
    /// At most [`MAX_PANELS`] panels are allowed and `max_reflections` must be
    /// in `0..=`[`MAX_REFLECTIONS`]. Every vector must be finite, the half-edge
    /// vectors must be nonzero and orthogonal, and every reflectance channel
    /// must lie in `[0, 1]`. Panels are assumed not to intersect or overlap;
    /// callers are responsible for guaranteeing that.
    pub fn new(panels: Vec<MirrorPanel>, max_reflections: usize) -> Result<Self, String> {
        if panels.len() > MAX_PANELS {
            return Err(format!(
                "expected at most {} mirror panels, found {}",
                MAX_PANELS,
                panels.len()
            ));
        }
        if max_reflections > MAX_REFLECTIONS {
            return Err(format!(
                "max_reflections must be at most {}, found {}",
                MAX_REFLECTIONS, max_reflections
            ));
        }
        for (index, panel) in panels.iter().enumerate() {
            let finite = [panel.center, panel.edge_u, panel.edge_v]
                .iter()
                .all(|v| v.iter().all(|component| component.is_finite()));
            if !finite {
                return Err(format!("panel {} contains a non-finite vector", index));
            }
            let edge_len = |edge: glm::DVec3| edge.magnitude_squared();
            if edge_len(panel.edge_u) <= LENGTH_EPS || edge_len(panel.edge_v) <= LENGTH_EPS {
                return Err(format!("panel {} has a zero-length half-edge", index));
            }
            let orthogonality = panel
                .edge_u
                .dot(&panel.edge_v)
                .abs()
                .max(PARALLEL_EPS)
                / (panel.edge_u.magnitude() * panel.edge_v.magnitude());
            if orthogonality > 1e-9 {
                return Err(format!("panel {} half-edges are not orthogonal", index));
            }
            for channel in [panel.reflectance.x, panel.reflectance.y, panel.reflectance.z] {
                if !channel.is_finite() || !(0.0..=1.0).contains(&channel) {
                    return Err(format!(
                        "panel {} reflectance channels must all be in [0, 1]",
                        index
                    ));
                }
            }
        }
        Ok(Self {
            panels,
            max_reflections,
        })
    }

    /// Read-only access to the validated panels.
    pub fn panels(&self) -> &[MirrorPanel] {
        &self.panels
    }

    /// Maximum number of reflections allowed in one point-light connection.
    pub fn max_reflections(&self) -> usize {
        self.max_reflections
    }

    /// Find every valid point-light mirror connection reaching `receiver`.
    ///
    /// Each returned [`MirrorConnection`] describes one ordered panel sequence
    /// of length at most `budget` and at most `max_reflections`, together with
    /// the actual world-space polyline that contributes illumination. The same
    /// returned paths are used by the renderer: there is no separate diagnostic
    /// solver.
    ///
    /// Direct (zero-reflection) lighting is not included here; it remains the
    /// renderer's responsibility. Consecutive repeats of the same panel are not
    /// enumerated because they collapse to a zero-length fold, while
    /// non-adjacent repeats such as `A -> B -> A` are explored fully.
    pub fn connect(
        &self,
        scene: &Scene,
        receiver: glm::DVec3,
        light: glm::DVec3,
        budget: usize,
    ) -> Vec<MirrorConnection> {
        let limit = self.max_reflections.min(budget);
        if limit == 0 || self.panels.is_empty() {
            return Vec::new();
        }
        let mut connections = Vec::new();
        let mut sequence = Vec::with_capacity(limit);
        for first in 0..self.panels.len() {
            sequence.push(first);
            self.enumerate(scene, receiver, light, limit, &mut sequence, &mut connections);
            sequence.pop();
        }
        connections
    }

    fn enumerate(
        &self,
        scene: &Scene,
        receiver: glm::DVec3,
        light: glm::DVec3,
        limit: usize,
        sequence: &mut Vec<usize>,
        connections: &mut Vec<MirrorConnection>,
    ) {
        if let Some(connection) = self.evaluate(scene, receiver, light, sequence) {
            connections.push(connection);
        }
        if sequence.len() == limit {
            return;
        }
        let last = *sequence.last().unwrap();
        for next in 0..self.panels.len() {
            // Consecutive hits of the same plane collapse into a zero-length fold.
            if next == last {
                continue;
            }
            sequence.push(next);
            self.enumerate(scene, receiver, light, limit, sequence, connections);
            sequence.pop();
        }
    }

    fn evaluate(
        &self,
        scene: &Scene,
        receiver: glm::DVec3,
        light: glm::DVec3,
        sequence: &[usize],
    ) -> Option<MirrorConnection> {
        // Build the chain of virtual mirror images of the light.
        let mut images = Vec::with_capacity(sequence.len() + 1);
        images.push(light);
        for &panel_index in sequence {
            let previous = *images.last().unwrap();
            images.push(self.panels[panel_index].reflect_point(&previous));
        }

        // Unfold from the receiver back toward the virtual images.
        let mut vertices = Vec::with_capacity(sequence.len());
        let mut current = receiver;
        for step in (0..sequence.len()).rev() {
            let panel = &self.panels[sequence[step]];
            let fold = panel.intersect_segment(&current, &images[step + 1])?;
            vertices.push(fold);
            current = fold;
        }
        vertices.reverse();

        // Assemble the full polyline: light, fold points, receiver.
        let mut points = Vec::with_capacity(sequence.len() + 2);
        points.push(light);
        points.extend(vertices.iter().copied());
        points.push(receiver);

        let mut total_length = 0.0;
        for segment in points.windows(2) {
            let length = (segment[1] - segment[0]).magnitude();
            if length <= LENGTH_EPS {
                return None;
            }
            total_length += length;
        }

        // Occlusion on the open segment: endpoint self-intersections are ignored,
        // ordinary objects and any panel other than the segment endpoints block.
        for segment in points.windows(2) {
            let (start, end) = (segment[0], segment[1]);
            let direction = end - start;
            let length = direction.magnitude();
            let ray = Ray {
                origin: start,
                dir: direction / length,
            };
            let far = length - LENGTH_EPS;
            for object in &scene.objects {
                let mut record = crate::shape::HitRecord::new();
                if object.shape.intersect(&ray, LENGTH_EPS, &mut record) && record.time < far {
                    return None;
                }
            }
            for panel in &self.panels {
                if let Some((time, _)) = panel.intersect_ray(&ray) {
                    if time > LENGTH_EPS && time < far {
                        return None;
                    }
                }
            }
        }

        let mut reflectance_product = glm::vec3(1.0, 1.0, 1.0);
        for &panel_index in sequence {
            reflectance_product = reflectance_product
                .component_mul(&self.panels[panel_index].reflectance);
        }

        Some(MirrorConnection {
            panels: sequence.to_vec(),
            vertices,
            total_length,
            reflectance_product,
        })
    }
}

/// One valid mirror path from a point light to a receiver.
///
/// The fields are read-only snapshots of the exact polyline the renderer uses
/// to accumulate illumination. `panels[k]` is the panel struck at
/// `vertices[k]`, with light flowing from the light through `vertices[0]`,
/// `vertices[1]`, ... to the receiver.
#[derive(Clone, Debug)]
pub struct MirrorConnection {
    /// Ordered panel indices, in the order the light strikes them.
    pub panels: Vec<usize>,

    /// World-space fold points on the panels, one per reflection.
    pub vertices: Vec<glm::DVec3>,

    /// Total unfolded length of the complete polyline.
    pub total_length: f64,

    /// Component-wise product of every reflection's RGB reflectance.
    pub reflectance_product: Color,
}
