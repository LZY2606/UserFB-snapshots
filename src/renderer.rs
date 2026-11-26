use image::RgbImage;
use rand::{rngs::StdRng, Rng, SeedableRng};
use rayon::prelude::*;

use crate::buffer::{Buffer, Filter};
use crate::camera::Camera;
use crate::color::Color;
use crate::light::Light;
use crate::material::Material;
use crate::mirrors::MirrorTransport;
use crate::object::Object;
use crate::scene::Scene;
use crate::shape::{HitRecord, Ray};

const EPSILON: f64 = 1e-12;
const FIREFLY_CLAMP: f64 = 100.0;

/// Builder object for rendering a scene
pub struct Renderer<'a> {
    /// The scene to be rendered
    pub scene: &'a Scene,

    /// The camera to use
    pub camera: Camera,

    /// The width of the output image
    pub width: u32,

    /// The height of the output image
    pub height: u32,

    /// Exposure value (EV)
    pub exposure_value: f64,

    /// Optional noise-reduction filter
    pub filter: Filter,

    /// The maximum number of ray bounces
    pub max_bounces: u32,

    /// Number of random paths traced per pixel
    pub num_samples: u32,

    /// Optional ideal mirror panels participating in eye paths and occlusion
    mirror_transport: Option<&'a MirrorTransport>,
}

impl<'a> Renderer<'a> {
    /// Construct a new renderer for a scene
    pub fn new(scene: &'a Scene, camera: Camera) -> Self {
        Self {
            scene,
            camera,
            width: 800,
            height: 600,
            exposure_value: 0.0,
            filter: Filter::default(),
            max_bounces: 0,
            num_samples: 1,
            mirror_transport: None,
        }
    }

    /// Set the width of the rendered scene
    pub fn width(mut self, width: u32) -> Self {
        self.width = width;
        self
    }

    /// Set the height of the rendered scene
    pub fn height(mut self, height: u32) -> Self {
        self.height = height;
        self
    }

    /// Set the exposure value of the rendered scene
    pub fn exposure_value(mut self, exposure_value: f64) -> Self {
        self.exposure_value = exposure_value;
        self
    }

    /// Set the noise reduction filter
    pub fn filter(mut self, filter: Filter) -> Self {
        self.filter = filter;
        self
    }

    /// Set the maximum number of ray bounces when ray is traced
    pub fn max_bounces(mut self, max_bounces: u32) -> Self {
        self.max_bounces = max_bounces;
        self
    }

    /// Set the number of random paths traced per pixel
    pub fn num_samples(mut self, num_samples: u32) -> Self {
        self.num_samples = num_samples;
        self
    }

    /// Attach ideal mirror panels, which become real geometry for eye paths,
    /// shadow rays and deterministic point-light connections
    ///
    /// When unset, rendering behaves exactly as before, including the random
    /// sampling sequence. Panels must not also be added to `Scene.objects`.
    pub fn mirror_transport(mut self, mirror_transport: &'a MirrorTransport) -> Self {
        self.mirror_transport = Some(mirror_transport);
        self
    }

    /// Render the scene by path tracing
    pub fn render(&self) -> RgbImage {
        let mut buffer = Buffer::new(self.width, self.height, self.filter);
        self.sample(self.num_samples, &mut buffer);
        buffer.image()
    }

    /// Render the scene iteratively, calling a callback after every k samples
    pub fn iterative_render<F>(&self, callback_interval: u32, mut callback: F)
    where
        F: FnMut(u32, &Buffer),
    {
        let mut buffer = Buffer::new(self.width, self.height, self.filter);
        let mut iteration = 0;
        while iteration < self.num_samples {
            let steps = std::cmp::min(self.num_samples - iteration, callback_interval);
            self.sample(steps, &mut buffer);
            iteration += steps;
            callback(iteration, &buffer);
        }
    }

    fn sample(&self, iterations: u32, buffer: &mut Buffer) {
        let colors: Vec<_> = (0..self.height)
            .into_par_iter()
            .flat_map(|y| {
                let mut rng = StdRng::from_entropy();
                (0..self.width)
                    .into_iter()
                    .map(|x| self.get_color(x, y, iterations, &mut rng))
                    .collect::<Vec<_>>()
            })
            .collect();
        buffer.add_samples(&colors);
    }

    fn get_color(&self, x: u32, y: u32, iterations: u32, rng: &mut StdRng) -> Color {
        let dim = std::cmp::max(self.width, self.height) as f64;
        let xn = ((2 * x + 1) as f64 - self.width as f64) / dim;
        let yn = ((2 * (self.height - y) - 1) as f64 - self.height as f64) / dim;
        let mut color = glm::vec3(0.0, 0.0, 0.0);
        for _ in 0..iterations {
            let dx = rng.gen_range((-1.0 / dim)..(1.0 / dim));
            let dy = rng.gen_range((-1.0 / dim)..(1.0 / dim));
            color += self.trace(self.camera.cast_ray(xn + dx, yn + dy, rng), 0, rng);
        }
        color / f64::from(iterations) * 2.0_f64.powf(self.exposure_value)
    }

    /// Trace a ray with a fixed seed, obtaining its linear, un-tonemapped color
    ///
    /// This delegates to the same tracing code used during rendering; no
    /// exposure, filtering or gamma correction is applied.
    pub fn trace_ray_seeded(&self, ray: Ray, seed: u64) -> Color {
        let mut rng = StdRng::seed_from_u64(seed);
        self.trace(ray, 0, &mut rng)
    }

    /// Trace a ray, obtaining a Monte Carlo estimate of the luminance
    fn trace(&self, ray: Ray, num_bounces: u32, rng: &mut StdRng) -> Color {
        match self.closest_surface(ray) {
            None => self.scene.environment.get_color(&ray.dir),
            Some(SurfaceHit::Mirror(hit_record, panel_index)) => {
                if num_bounces >= self.max_bounces {
                    return glm::vec3(0.0, 0.0, 0.0);
                }
                let panel = &self.mirror_transport.unwrap().panels()[panel_index];
                let world_pos = ray.at(hit_record.time);
                let normal = hit_record.normal;
                // Ideal specular reflection; the panel normal faces the incoming ray.
                let reflected =
                    ray.dir - 2.0 * ray.dir.dot(&normal) * normal;
                let reflected_ray = Ray {
                    origin: world_pos,
                    dir: reflected.normalize(),
                };
                let incoming = self.trace(reflected_ray, num_bounces + 1, rng);
                panel.reflectance.component_mul(&incoming)
            }
            Some(SurfaceHit::Object(h, object)) => {
                let world_pos = ray.at(h.time);
                let material = object.material;
                let wo = -glm::normalize(&ray.dir);

                let mut color = material.emittance * material.color;
                color += self.sample_lights(&material, &world_pos, &h.normal, &wo, num_bounces, rng);
                if num_bounces < self.max_bounces {
                    if let Some((wi, pdf)) = material.sample_f(&h.normal, &wo, rng) {
                        let f = material.bsdf(&h.normal, &wo, &wi);
                        let ray = Ray {
                            origin: world_pos,
                            dir: wi,
                        };
                        let indirect = 1.0 / pdf
                            * f.component_mul(&self.trace(ray, num_bounces + 1, rng))
                            * wi.dot(&h.normal).abs();
                        color.x += indirect.x.min(FIREFLY_CLAMP);
                        color.y += indirect.y.min(FIREFLY_CLAMP);
                        color.z += indirect.z.min(FIREFLY_CLAMP);
                    }
                }

                color
            }
        }
    }

    /// Explicitly sample from all the lights in the scene
    fn sample_lights(
        &self,
        material: &Material,
        pos: &glm::DVec3,
        n: &glm::DVec3,
        wo: &glm::DVec3,
        num_bounces: u32,
        rng: &mut StdRng,
    ) -> Color {
        let mut color = glm::vec3(0.0, 0.0, 0.0);
        for light in &self.scene.lights {
            if let Light::Ambient(ambient_color) = light {
                color += ambient_color.component_mul(&material.color);
            } else if let Light::Point(intensity, light_pos) = light {
                let (attenuated, wi, dist_to_light) = light.illuminate(pos, rng);
                let occluded = matches!(
                    self.closest_surface(Ray {
                        origin: *pos,
                        dir: wi,
                    }),
                    Some(hit) if hit.time() <= dist_to_light
                );
                if !occluded {
                    let f = material.bsdf(n, wo, &wi);
                    color += f.component_mul(&attenuated) * wi.dot(n);
                }

                // Deterministic point-light connections through mirror panels.
                // They share the eye-path bounce budget, capped further by the
                // transport's per-connection reflection limit.
                if let Some(transport) = self.mirror_transport {
                    let budget = self.max_bounces.saturating_sub(num_bounces) as usize;
                    for connection in transport.connect(self.scene, *pos, *light_pos, budget) {
                        // Direction of the first polyline segment, at the receiver.
                        let wi_mirror = (connection.vertices[0] - pos).normalize();
                        let cosine = wi_mirror.dot(n);
                        if cosine <= 0.0 {
                            continue;
                        }
                        let f = material.bsdf(n, wo, &wi_mirror);
                        let path_attenuation =
                            *intensity / connection.total_length.powi(2);
                        let contribution = f
                            .component_mul(&path_attenuation)
                            .component_mul(&connection.reflectance_product)
                            * cosine;
                        color += contribution;
                    }
                }
            } else {
                let (intensity, wi, dist_to_light) = light.illuminate(pos, rng);
                let occluded = matches!(
                    self.closest_surface(Ray {
                        origin: *pos,
                        dir: wi,
                    }),
                    Some(hit) if hit.time() <= dist_to_light
                );
                if !occluded {
                    let f = material.bsdf(n, wo, &wi);
                    color += f.component_mul(&intensity) * wi.dot(n);
                }
            }
        }
        color
    }

    /// Find the closest surface hit, treating mirror panels as real geometry.
    fn closest_surface(&self, ray: Ray) -> Option<SurfaceHit<'_>> {
        let mut h = HitRecord::new();
        let mut hit_object: Option<&Object> = None;
        for object in &self.scene.objects {
            if object.shape.intersect(&ray, EPSILON, &mut h) {
                hit_object = Some(object);
            }
        }
        let mut result = hit_object.map(|object| SurfaceHit::Object(h, object));
        if let Some(transport) = self.mirror_transport {
            for (panel_index, panel) in transport.panels().iter().enumerate() {
                if let Some((time, normal)) = panel.intersect_ray(&ray) {
                    let closer = match &result {
                        None => true,
                        Some(hit) => time < hit.time(),
                    };
                    if closer {
                        result = Some(SurfaceHit::Mirror(
                            HitRecord { time, normal },
                            panel_index,
                        ));
                    }
                }
            }
        }
        result
    }
}

/// Closest-hit result: either an ordinary object or an ideal mirror panel.
enum SurfaceHit<'a> {
    Object(HitRecord, &'a Object),
    Mirror(HitRecord, usize),
}

impl SurfaceHit<'_> {
    /// Ray parameter at the hit.
    fn time(&self) -> f64 {
        match self {
            Self::Object(record, _) => record.time,
            Self::Mirror(record, _) => record.time,
        }
    }
}
