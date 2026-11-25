//! Finite planar mirror panels and deterministic specular light connections.
//!
//! A [`MirrorTransport`] holds a small set of finite, double-sided, ideal
//! planar mirror panels. Once configured on a [`crate::Renderer`], the panels
//! act as real geometry: eye rays reflect off them, they occlude shadow rays,
//! and point lights are connected to shaded points through ordered sequences
//! of mirror reflections. Connections are found analytically by unfolding the
//! mirror planes (the virtual-image method), so no random direction sampling
//! is required to discover these discrete light paths.
//!
//! # Degenerate configurations
//!
//! The following degenerate cases produce no connection and no contribution:
//!
//! - an unfolded segment that is parallel to the next mirror plane (this
//!   includes coplanar light or receiver points),
//! - a fold point that falls outside the finite mirror aperture,
//! - a zero-length segment (coincident light, receiver, or fold points).
//!
//! # Finite segment tolerance
//!
//! Visibility along each segment of a connection is tested on the open
//! segment: intersections within `SEGMENT_EPSILON` of either endpoint are
//! ignored. This excludes self-intersection at the two panels (or the
//! receiver surface) that terminate the segment, while any other object or
//! mirror panel crossing the interior of a segment rejects the connection.
//! The mirror aperture is inclusive: fold points on the panel boundary count
//! as inside, with a tolerance of `APERTURE_TOL` in normalized coordinates.

use crate::color::Color;
use crate::scene::Scene;
use crate::shape::{HitRecord, Ray};

/// Maximum number of mirror panels in a transport
pub const MAX_PANELS: usize = 8;

/// Maximum number of mirror reflections in a single connection
pub const MAX_REFLECTIONS: usize = 4;

/// Tolerance for the aperture test, in normalized panel coordinates
const APERTURE_TOL: f64 = 1e-9;

/// Margin excluded at both ends of a finite visibility segment
const SEGMENT_EPSILON: f64 = 1e-9;

/// Minimum length of a polyline segment; shorter segments are degenerate
const MIN_SEGMENT_LEN: f64 = 1e-9;

/// Threshold below which a direction is considered parallel to a mirror plane
const PLANE_PARALLEL_EPS: f64 = 1e-12;

/// Relative tolerance for the orthogonality of the panel edge vectors
const ORTHOGONAL_TOL: f64 = 1e-9;

/// A finite, double-sided, ideal planar mirror panel
///
/// The panel covers `center + a * edge_u + b * edge_v` for `a, b` in
/// `[-1, 1]`, where `edge_u` and `edge_v` are orthogonal, nonzero half-edge
/// vectors. Both sides of the panel reflect ideally, tinted per channel by
/// `reflectance`.
pub struct MirrorPanel {
    /// Center of the panel
    pub center: glm::DVec3,

    /// Half-edge vector spanning the first axis of the panel
    pub edge_u: glm::DVec3,

    /// Half-edge vector spanning the second axis of the panel
    pub edge_v: glm::DVec3,

    /// Per-channel mirror reflectance, each component in `[0, 1]`
    pub reflectance: Color,
}

impl MirrorPanel {
    /// Unit normal of the mirror plane
    ///
    /// The panel is double-sided, so either orientation of the normal
    /// describes the same reflecting plane.
    pub fn normal(&self) -> glm::DVec3 {
        glm::normalize(&glm::cross(&self.edge_u, &self.edge_v))
    }

    /// Normalized panel coordinates of a world point
    fn local_coords(&self, p: &glm::DVec3) -> (f64, f64) {
        let d = p - self.center;
        (
            d.dot(&self.edge_u) / self.edge_u.magnitude_squared(),
            d.dot(&self.edge_v) / self.edge_v.magnitude_squared(),
        )
    }

    /// Whether a world point lies inside the finite aperture
    ///
    /// The boundary of the aperture counts as inside, up to a small
    /// tolerance in normalized panel coordinates.
    pub fn contains_point(&self, p: &glm::DVec3) -> bool {
        let (a, b) = self.local_coords(p);
        a.abs() <= 1.0 + APERTURE_TOL && b.abs() <= 1.0 + APERTURE_TOL
    }

    /// Intersect a ray with the finite panel, returning the hit time
    ///
    /// Only hits strictly inside the open interval `(t_min, t_max)` and
    /// inside the finite aperture are returned.
    fn intersect(&self, ray: &Ray, t_min: f64, t_max: f64) -> Option<f64> {
        let n = self.normal();
        let denom = ray.dir.dot(&n);
        if denom.abs() < PLANE_PARALLEL_EPS {
            return None;
        }
        let t = (self.center - ray.origin).dot(&n) / denom;
        if t <= t_min || t >= t_max {
            return None;
        }
        if self.contains_point(&ray.at(t)) {
            Some(t)
        } else {
            None
        }
    }
}

/// A deterministic connection from a point light to a receiver point through
/// an ordered sequence of mirror panels
///
/// This is the exact path used for illumination; the renderer evaluates the
/// same connection that is exposed here, so observers always see the paths
/// that actually contribute light.
#[derive(Clone, Debug)]
pub struct MirrorConnection {
    /// Ordered sequence of panel indices, from the light to the receiver
    pub panels: Vec<usize>,

    /// World-space fold points on each panel, in the same order as `panels`
    pub points: Vec<glm::DVec3>,

    /// Total length of the polyline from the light to the receiver
    pub total_length: f64,

    /// Product of the per-channel reflectances of the visited panels
    pub reflectance: Color,
}

/// A set of ideal mirror panels participating in eye paths and point-light
/// connections
///
/// The transport is immutable after construction and can be shared by
/// renderers. Panels are real geometry: they are intersected by eye rays and
/// shadow rays, and they are not copied into [`Scene`] objects.
pub struct MirrorTransport {
    panels: Vec<MirrorPanel>,
    max_reflections: usize,
}

impl MirrorTransport {
    /// Create a mirror transport from panels and a per-connection limit on
    /// the number of mirror reflections
    ///
    /// Returns an error if there are more than 8 panels, if
    /// `max_reflections` exceeds 4, or if any panel is degenerate: non-finite
    /// parameters, zero-length or non-orthogonal edge vectors, or reflectance
    /// channels outside `[0, 1]`. Panels are assumed not to intersect or
    /// overlap each other; this is the caller's responsibility.
    pub fn new(panels: Vec<MirrorPanel>, max_reflections: usize) -> Result<Self, String> {
        if panels.len() > MAX_PANELS {
            return Err(format!(
                "too many mirror panels: {} > {}",
                panels.len(),
                MAX_PANELS
            ));
        }
        if max_reflections > MAX_REFLECTIONS {
            return Err(format!(
                "too many mirror reflections: {} > {}",
                max_reflections, MAX_REFLECTIONS
            ));
        }
        for (i, panel) in panels.iter().enumerate() {
            let finite = panel.center.iter().all(|c| c.is_finite())
                && panel.edge_u.iter().all(|c| c.is_finite())
                && panel.edge_v.iter().all(|c| c.is_finite())
                && panel.reflectance.iter().all(|c| c.is_finite());
            if !finite {
                return Err(format!("mirror panel {} has non-finite parameters", i));
            }
            let len_u = panel.edge_u.magnitude();
            let len_v = panel.edge_v.magnitude();
            if len_u == 0.0 || len_v == 0.0 {
                return Err(format!("mirror panel {} has a zero edge vector", i));
            }
            if panel.edge_u.dot(&panel.edge_v).abs() > ORTHOGONAL_TOL * len_u * len_v {
                return Err(format!("mirror panel {} edges are not orthogonal", i));
            }
            if panel.reflectance.iter().any(|&c| !(0.0..=1.0).contains(&c)) {
                return Err(format!(
                    "mirror panel {} reflectance is outside [0, 1]",
                    i
                ));
            }
        }
        Ok(Self {
            panels,
            max_reflections,
        })
    }

    /// The mirror panels of this transport
    pub fn panels(&self) -> &[MirrorPanel] {
        &self.panels
    }

    /// The per-connection limit on the number of mirror reflections
    pub fn max_reflections(&self) -> usize {
        self.max_reflections
    }

    /// Find the closest panel hit by a ray within `(t_min, t_max)`,
    /// returning the panel index and hit time
    pub fn raycast(&self, ray: &Ray, t_min: f64, t_max: f64) -> Option<(usize, f64)> {
        let mut best = None;
        let mut best_t = t_max;
        for (i, panel) in self.panels.iter().enumerate() {
            if let Some(t) = panel.intersect(ray, t_min, best_t) {
                best = Some((i, t));
                best_t = t;
            }
        }
        best
    }

    /// Find all valid mirror connections from a point light to a receiver
    /// point in a scene
    ///
    /// Ordered sequences of up to `min(budget, max_reflections)` mirrors are
    /// enumerated; consecutive repeats of the same panel are skipped, while
    /// non-adjacent repeats (such as A→B→A) are allowed. Each candidate
    /// sequence is solved exactly with the virtual-image method and validated
    /// against the finite apertures and the occlusion geometry of the scene.
    /// The direct (zero-mirror) path is not part of the result.
    pub fn find_connections(
        &self,
        scene: &Scene,
        receiver: &glm::DVec3,
        light: &glm::DVec3,
        budget: usize,
    ) -> Vec<MirrorConnection> {
        let limit = self.max_reflections.min(budget);
        let mut connections = Vec::new();
        if limit == 0 || self.panels.is_empty() {
            return connections;
        }
        let mut sequence = Vec::with_capacity(limit);
        self.enumerate(scene, receiver, light, limit, &mut sequence, &mut connections);
        connections
    }

    /// Depth-first enumeration of ordered mirror sequences
    fn enumerate(
        &self,
        scene: &Scene,
        receiver: &glm::DVec3,
        light: &glm::DVec3,
        limit: usize,
        sequence: &mut Vec<usize>,
        out: &mut Vec<MirrorConnection>,
    ) {
        if sequence.len() == limit {
            return;
        }
        for index in 0..self.panels.len() {
            // Consecutive reflections on the same panel cannot contribute;
            // non-adjacent repeats (A→B→A) are kept.
            if sequence.last() == Some(&index) {
                continue;
            }
            sequence.push(index);
            if let Some(connection) = self.solve(sequence, light, receiver) {
                if self.path_visible(scene, light, receiver, &connection) {
                    out.push(connection);
                }
            }
            self.enumerate(scene, receiver, light, limit, sequence, out);
            sequence.pop();
        }
    }

    /// Solve a mirror sequence exactly with the virtual-image method
    ///
    /// The light is reflected across the mirror planes in sequence order,
    /// producing virtual images. The fold points are then recovered back to
    /// front by intersecting straight lines with the mirror planes, which
    /// yields the unique polyline satisfying the law of reflection.
    fn solve(
        &self,
        sequence: &[usize],
        light: &glm::DVec3,
        receiver: &glm::DVec3,
    ) -> Option<MirrorConnection> {
        // Virtual images of the light, unfolding the mirror planes in order
        let mut images: Vec<glm::DVec3> = Vec::with_capacity(sequence.len() + 1);
        images.push(*light);
        for &index in sequence {
            let panel = &self.panels[index];
            let n = panel.normal();
            let prev = *images.last().unwrap();
            images.push(prev - 2.0 * (prev - panel.center).dot(&n) * n);
        }
        // Recover the fold points back to front
        let mut points = vec![glm::vec3(0.0, 0.0, 0.0); sequence.len()];
        let mut target = *receiver;
        for j in (0..sequence.len()).rev() {
            let panel = &self.panels[sequence[j]];
            let n = panel.normal();
            let image = images[j + 1];
            let d = target - image;
            let denom = d.dot(&n);
            if denom.abs() < PLANE_PARALLEL_EPS {
                return None; // unfolded segment parallel to the mirror plane
            }
            let t = (panel.center - image).dot(&n) / denom;
            if !(t > 1e-12 && t < 1.0 - 1e-12) {
                return None; // fold point not strictly between image and target
            }
            let point = image + t * d;
            if !panel.contains_point(&point) {
                return None; // outside the finite aperture
            }
            points[j] = point;
            target = point;
        }
        // Total polyline length, rejecting zero-length segments
        let mut total_length = 0.0;
        let mut prev = *light;
        for point in points.iter().chain(std::iter::once(receiver)) {
            let len = (*point - prev).magnitude();
            if len <= MIN_SEGMENT_LEN {
                return None;
            }
            total_length += len;
            prev = *point;
        }
        let mut reflectance = glm::vec3(1.0, 1.0, 1.0);
        for &index in sequence {
            reflectance = reflectance.component_mul(&self.panels[index].reflectance);
        }
        Some(MirrorConnection {
            panels: sequence.to_vec(),
            points,
            total_length,
            reflectance,
        })
    }

    /// Check that every segment of a connection is unobstructed
    fn path_visible(
        &self,
        scene: &Scene,
        light: &glm::DVec3,
        receiver: &glm::DVec3,
        connection: &MirrorConnection,
    ) -> bool {
        let mut prev = *light;
        for point in &connection.points {
            if !self.segment_visible(scene, &prev, point) {
                return false;
            }
            prev = *point;
        }
        self.segment_visible(scene, &prev, receiver)
    }

    /// Check a finite segment against scene objects and all mirror panels
    ///
    /// Intersections within `SEGMENT_EPSILON` of either endpoint are ignored,
    /// which excludes self-intersection at the geometry terminating the
    /// segment. Anything else crossing the segment interior, including
    /// mirror panels that are not part of this connection, blocks it.
    fn segment_visible(&self, scene: &Scene, a: &glm::DVec3, b: &glm::DVec3) -> bool {
        let d = *b - *a;
        let len = d.magnitude();
        if len <= 2.0 * SEGMENT_EPSILON {
            return false; // degenerate zero-length segment
        }
        let ray = Ray {
            origin: *a,
            dir: d / len,
        };
        let t_max = len - SEGMENT_EPSILON;
        let mut h = HitRecord::new();
        for object in &scene.objects {
            object.shape.intersect(&ray, SEGMENT_EPSILON, &mut h);
        }
        if h.time < t_max {
            return false;
        }
        for panel in &self.panels {
            if panel.intersect(&ray, SEGMENT_EPSILON, t_max).is_some() {
                return false;
            }
        }
        true
    }
}
