//! Renders a small scene with a finite planar mirror panel.
//!
//! A point light sits behind an occluder, so the floor patch near the origin
//! is lit only through the mirror. The image is written as a PNG to the path
//! given on the command line.

use rpt::mirrors::{MirrorPanel, MirrorTransport};
use rpt::*;

fn main() {
    let out = std::env::args()
        .nth(1)
        .expect("usage: mirror_demo <output.png>");

    let mut scene = Scene::new();
    // Floor
    scene.add(
        Object::new(plane(glm::vec3(0.0, 1.0, 0.0), 0.0))
            .material(Material::diffuse(glm::vec3(0.7, 0.7, 0.7))),
    );
    // Occluder blocking the direct path from the light to the origin
    scene.add(
        Object::new(
            sphere()
                .scale(&glm::vec3(0.9, 0.9, 0.9))
                .translate(&glm::vec3(1.5, 1.5, 0.0)),
        )
        .material(Material::diffuse(glm::vec3(0.4, 0.3, 0.2))),
    );
    scene.add(Light::Point(
        glm::vec3(40.0, 40.0, 40.0),
        glm::vec3(3.0, 3.0, 0.0),
    ));

    // Vertical mirror panel on the left, slightly warm reflectance
    let mirrors = MirrorTransport::new(
        vec![MirrorPanel {
            center: glm::vec3(-4.0, 2.0, 0.0),
            edge_u: glm::vec3(0.0, 2.0, 0.0),
            edge_v: glm::vec3(0.0, 0.0, 2.0),
            reflectance: glm::vec3(0.9, 0.85, 0.8),
        }],
        4,
    )
    .expect("valid mirror transport");

    // The floor point at the origin is reached by the light only via the mirror
    let connections = mirrors.find_connections(
        &scene,
        &glm::vec3(0.0, 0.0, 0.0),
        &glm::vec3(3.0, 3.0, 0.0),
        4,
    );
    assert!(!connections.is_empty(), "expected a mirror connection");
    println!(
        "mirror connection: panels={:?} length={:.4}",
        connections[0].panels, connections[0].total_length
    );

    let camera = Camera::look_at(
        glm::vec3(0.0, 5.0, 9.0),
        glm::vec3(0.0, 0.5, 0.0),
        glm::vec3(0.0, 1.0, 0.0),
        0.6,
    );
    let image = Renderer::new(&scene, camera)
        .width(96)
        .height(64)
        .num_samples(4)
        .max_bounces(3)
        .mirror_transport(&mirrors)
        .render();

    let mut sum: u64 = 0;
    for pixel in image.pixels() {
        sum += pixel.0.iter().map(|&c| u64::from(c)).sum::<u64>() as u64;
    }
    assert!(sum > 0, "rendered image is black");
    println!("rendered 96x64 image, byte sum = {}", sum);

    image.save(&out).expect("failed to save PNG");
    println!("wrote {}", out);
}
