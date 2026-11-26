// End-to-end mirror demo: a point light is hidden from a ground patch by an
// occluder, but a finite vertical mirror folds light onto it. The rendered
// image is written to the path given as the first command-line argument.
use rpt::glm;
use rpt::{
    cube, Camera, Light, Material, MirrorPanel, MirrorTransport, Object, Plane, Renderer, Scene,
    SceneAdd, Transformable,
};

fn main() {
    let output_path = std::env::args()
        .nth(1)
        .expect("usage: mirror_demo <output.png>");

    let mut scene = Scene::new();

    // Ground plane, y = 0.
    scene.add(
        Object::new(Plane {
            normal: glm::vec3(0.0, 1.0, 0.0),
            value: 0.0,
        })
        .material(Material::diffuse(glm::vec3(0.7, 0.7, 0.7))),
    );

    // Occluder on the direct light path above the origin.
    scene.add(
        Object::new(cube().scale(&glm::vec3(0.6, 0.6, 0.6)).translate(
            &glm::vec3(0.0, 1.5, 0.0),
        ))
        .material(Material::diffuse(glm::vec3(0.2, 0.4, 0.9))),
    );

    // Point light directly above the occluder.
    let light_pos = glm::vec3(0.0, 4.0, 0.0);
    let light_intensity = glm::vec3(80.0, 80.0, 80.0);
    scene.add(Light::Point(light_intensity, light_pos));

    // Finite vertical mirror at x = 2, spanning y, z in [-2, 2] around center.
    let panel = MirrorPanel {
        center: glm::vec3(2.0, 3.0, 0.0),
        edge_u: glm::vec3(0.0, 2.0, 0.0),
        edge_v: glm::vec3(0.0, 0.0, 2.0),
        reflectance: glm::vec3(0.9, 0.8, 0.6),
    };
    let transport = MirrorTransport::new(vec![panel], 4).expect("valid mirror transport");

    // The ground point under the occluder must be dark directly but lit via
    // the folded mirror path.
    let receiver = glm::vec3(0.0, 0.0, 0.0);
    let connections = transport.connect(&scene, receiver, light_pos, 4);
    assert!(!connections.is_empty(), "mirror connection must exist");
    let connection = &connections[0];
    assert_eq!(connection.panels, vec![0]);
    assert_eq!(connection.vertices.len(), 1);
    let fold = connection.vertices[0];
    assert!((fold - glm::vec3(2.0, 2.0, 0.0)).magnitude() < 1e-8);
    assert!(connection.total_length > 0.0);
    println!(
        "MIRROR_DEMO fold=({:.3},{:.3},{:.3}) total_length={:.3} reflectance=({:.2},{:.2},{:.2})",
        fold.x,
        fold.y,
        fold.z,
        connection.total_length,
        connection.reflectance_product.x,
        connection.reflectance_product.y,
        connection.reflectance_product.z
    );

    let camera = Camera::look_at(
        glm::vec3(0.0, 2.5, 8.0),
        glm::vec3(0.5, 2.0, 0.0),
        glm::vec3(0.0, 1.0, 0.0),
        1.0,
    );
    let image = Renderer::new(&scene, camera)
        .mirror_transport(&transport)
        .width(32)
        .height(32)
        .num_samples(4)
        .max_bounces(2)
        .render();

    assert_eq!(image.width(), 32);
    assert_eq!(image.height(), 32);
    let total: u64 = image
        .pixels()
        .map(|pixel| {
            pixel
                .0
                .iter()
                .map(|component| u64::from(*component))
                .sum::<u64>()
        })
        .sum();
    assert!(total > 0, "rendered image must be finite and non-black");
    println!("MIRROR_DEMO rendered 32x32 nonblack pixel_sum={}", total);

    // The seeded entry point delegates to the real tracer and returns linear color.
    let linear = Renderer::new(&scene, camera)
        .mirror_transport(&transport)
        .max_bounces(2)
        .trace_ray_seeded(
            rpt::Ray {
                origin: camera.eye,
                dir: camera.direction,
            },
            42,
        );
    assert!(linear.iter().all(|component| component.is_finite()));
    println!(
        "MIRROR_DEMO seeded_linear=({:.4},{:.4},{:.4})",
        linear.x, linear.y, linear.z
    );

    image
        .save(&output_path)
        .expect("failed to write output PNG");
    println!("MIRROR_DEMO wrote {}", output_path);
}
