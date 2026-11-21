// Finite planar mirror demo: a point light reaches a ground receiver through a
// deterministic, folded specular path. Renders a small non-black PNG to the path
// given as the first command-line argument. No GPU, window or external assets.
use rpt::*;

fn main() {
    let output_path = std::env::args()
        .nth(1)
        .expect("usage: mirror_demo <output.png>");

    let mut scene = Scene::new();

    // Ordinary diffuse ground receiver (z = 0).
    scene.add(
        Object::new(Plane {
            normal: glm::vec3(0.0, 0.0, 1.0),
            value: 0.0,
        })
        .material(Material::diffuse(glm::vec3(0.6, 0.6, 0.6))),
    );

    // Strong point light on the +x side of a vertical mirror.
    scene.add(Light::Point(
        glm::vec3(90.0, 90.0, 90.0),
        glm::vec3(1.5, 0.0, 2.5),
    ));

    // Finite vertical mirror occupying x = 0, y in [-1.5, 1.5], z in [0, 2.5].
    let panel = MirrorPanel {
        center: glm::vec3(0.0, 0.0, 1.25),
        edge_u: glm::vec3(0.0, 1.5, 0.0),
        edge_v: glm::vec3(0.0, 0.0, 1.25),
        reflectance: glm::vec3(0.9, 0.7, 0.45),
    };
    let transport = MirrorTransport::new(vec![panel], 2).expect("valid mirror configuration");

    let camera = Camera::look_at(
        glm::vec3(2.6, -3.4, 2.4),
        glm::vec3(0.3, 0.0, 0.9),
        glm::vec3(0.0, 0.0, 1.0),
        1.0,
    );

    let image = Renderer::new(&scene, camera)
        .mirror_transport(&transport)
        .width(64)
        .height(48)
        .num_samples(8)
        .max_bounces(3)
        .render();

    image.save(&output_path).expect("failed to write PNG");

    let sum: u64 = image
        .pixels()
        .map(|pixel| pixel.0.iter().map(|x| u64::from(*x)).sum::<u64>())
        .sum();
    assert!(sum > 0, "rendered mirror demo must be non-black");
    println!(
        "MIRROR_DEMO saved={} pixels={} nonblack_sum={}",
        output_path,
        image.width() * image.height(),
        sum
    );
}
