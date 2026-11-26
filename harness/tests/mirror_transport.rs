//! Integration tests for deterministic ideal-mirror transport.
//!
//! Every numeric expectation is derived independently in the test (mirror
//! images, 2D cross-section line intersections, or the receiver's own BSDF)
//! rather than read back from the solver under test.
#![allow(clippy::many_single_char_names)]

#[path = "../../task_support/check.rs"]
mod taskcheck;

use rpt::glm;
use rpt::{
    cube, sphere, Camera, Environment, Light, Material, MirrorPanel, MirrorTransport, Object,
    Plane, Ray, Renderer, Scene, SceneAdd, Transformable,
};

fn white() -> glm::DVec3 {
    glm::vec3(1.0, 1.0, 1.0)
}

fn panel(
    center: glm::DVec3,
    edge_u: glm::DVec3,
    edge_v: glm::DVec3,
    reflectance: glm::DVec3,
) -> MirrorPanel {
    MirrorPanel {
        center,
        edge_u,
        edge_v,
        reflectance,
    }
}

fn find_connection<'a>(
    connections: &'a [rpt::MirrorConnection],
    sequence: &[usize],
) -> Option<&'a rpt::MirrorConnection> {
    connections
        .iter()
        .find(|connection| connection.panels == sequence)
}

fn vec3_close(a: glm::DVec3, b: glm::DVec3, tolerance: f64) -> bool {
    (a - b).magnitude() <= tolerance
}

#[test]
fn mirror_geometry() {
    let g = taskcheck::Group::new("mirror_geometry");
    let empty_scene = Scene::new();

    // ----- Constructor validation -------------------------------------------
    g.check(
        MirrorTransport::new(vec![], 4).is_ok(),
        "empty panel set is valid",
    );
    let too_many = (0..9)
        .map(|_| {
            panel(
                glm::vec3(0.0, 0.0, 0.0),
                glm::vec3(1.0, 0.0, 0.0),
                glm::vec3(0.0, 1.0, 0.0),
                white(),
            )
        })
        .collect();
    g.check(
        MirrorTransport::new(too_many, 4).is_err(),
        "more than 8 panels rejected",
    );
    g.check(
        MirrorTransport::new(
            vec![panel(
                glm::vec3(0.0, 0.0, 0.0),
                glm::vec3(1.0, 0.0, 0.0),
                glm::vec3(0.0, 1.0, 0.0),
                white(),
            )],
            5,
        )
        .is_err(),
        "max_reflections above 4 rejected",
    );
    g.check(
        MirrorTransport::new(
            vec![panel(
                glm::vec3(0.0, 0.0, 0.0),
                glm::vec3(0.0, 0.0, 0.0),
                glm::vec3(0.0, 1.0, 0.0),
                white(),
            )],
            4,
        )
        .is_err(),
        "zero half-edge rejected",
    );
    g.check(
        MirrorTransport::new(
            vec![panel(
                glm::vec3(0.0, 0.0, 0.0),
                glm::vec3(1.0, 0.0, 0.0),
                glm::vec3(0.5, 1.0, 0.0),
                white(),
            )],
            4,
        )
        .is_err(),
        "non-orthogonal half-edges rejected",
    );
    g.check(
        MirrorTransport::new(
            vec![panel(
                glm::vec3(0.0, 0.0, 0.0),
                glm::vec3(1.0, 0.0, 0.0),
                glm::vec3(0.0, 1.0, 0.0),
                glm::vec3(0.5, 1.2, 0.0),
            )],
            4,
        )
        .is_err(),
        "reflectance outside [0, 1] rejected",
    );
    g.check(
        MirrorTransport::new(
            vec![panel(
                glm::vec3(f64::NAN, 0.0, 0.0),
                glm::vec3(1.0, 0.0, 0.0),
                glm::vec3(0.0, 1.0, 0.0),
                white(),
            )],
            4,
        )
        .is_err(),
        "non-finite center rejected",
    );

    // ----- One mirror against an independent 2D reference -------------------
    // Light L=(0,4), receiver R=(0,0), mirror plane x=2. Mirror image of the
    // light is L'=(4,4); the line R->L' meets the plane at P=(2,2).
    let light = glm::vec3(0.0, 4.0, 0.0);
    let receiver = glm::vec3(0.0, 0.0, 0.0);
    let mirror_a = panel(
        glm::vec3(2.0, 2.0, 0.0),
        glm::vec3(0.0, 2.0, 0.0),
        glm::vec3(0.0, 0.0, 0.5),
        glm::vec3(0.7, 0.6, 0.5),
    );
    let transport = MirrorTransport::new(vec![mirror_a], 4).unwrap();
    let connections = transport.connect(&empty_scene, receiver, light, 4);
    let one = find_connection(&connections, &[0]).expect("one-mirror connection");
    g.check(one.vertices.len() == 1, "one fold point for one reflection");
    g.check(
        vec3_close(one.vertices[0], glm::vec3(2.0, 2.0, 0.0), 1e-9),
        "fold matches 2D mirror-image reference (2,2,0)",
    );
    let expected_length = 4.0 * 2.0_f64.sqrt();
    g.check(
        (one.total_length - expected_length).abs() < 1e-9,
        "total length is the unfolded length 4*sqrt(2)",
    );
    g.check(
        vec3_close(
            one.reflectance_product,
            glm::vec3(0.7, 0.6, 0.5),
            1e-12,
        ),
        "reflectance product stored on the connection",
    );
    // Independent reflection-law check at the fold.
    let normal = mirror_a.normal();
    let d_in = (one.vertices[0] - light).normalize();
    let d_out = (receiver - one.vertices[0]).normalize();
    g.check(
        (normal.dot(&d_in) + normal.dot(&d_out)).abs() < 1e-10,
        "incident and reflected directions obey the reflection law",
    );
    // Independent collinearity of receiver, fold and virtual image.
    let virtual_image = {
        let distance = normal.dot(&(light - mirror_a.center));
        light - 2.0 * distance * normal
    };
    g.check(
        vec3_close(virtual_image, glm::vec3(4.0, 4.0, 0.0), 1e-9),
        "independently computed virtual image is (4,4,0)",
    );
    let to_image = (virtual_image - receiver).normalize();
    let to_fold = (one.vertices[0] - receiver).normalize();
    g.check(
        (to_image - to_fold).magnitude() < 1e-10,
        "receiver, fold and virtual image are collinear",
    );

    // ----- Non-parallel (perpendicular) two-mirror corner -------------------
    // Panel A on plane x=0, panel B on plane y=0; L=(2,2), R=(3,1).
    // Mirror images for sequence A then B: L1=(-2,2), L2=(-2,-2).
    // Folds derived by hand: on B at (4/3, 0), on A at (0, 0.8).
    let corner_a = panel(
        glm::vec3(0.0, 2.0, 0.0),
        glm::vec3(0.0, 3.0, 0.0),
        glm::vec3(0.0, 0.0, 0.5),
        white(),
    );
    let corner_b = panel(
        glm::vec3(2.0, 0.0, 0.0),
        glm::vec3(3.0, 0.0, 0.0),
        glm::vec3(0.0, 0.0, 0.5),
        white(),
    );
    let corner = MirrorTransport::new(vec![corner_a, corner_b], 4).unwrap();
    let corner_light = glm::vec3(2.0, 2.0, 0.0);
    let corner_receiver = glm::vec3(3.0, 1.0, 0.0);
    let corner_connections =
        corner.connect(&empty_scene, corner_receiver, corner_light, 4);
    let two = find_connection(&corner_connections, &[0, 1])
        .expect("two-reflection corner connection");
    g.check(
        vec3_close(two.vertices[0], glm::vec3(0.0, 0.8, 0.0), 1e-9),
        "first fold on panel A at the hand-derived (0, 0.8)",
    );
    g.check(
        vec3_close(
            two.vertices[1],
            glm::vec3(4.0 / 3.0, 0.0, 0.0),
            1e-9,
        ),
        "second fold on panel B at the hand-derived (4/3, 0)",
    );
    g.check(
        corner_a.normal().dot(&corner_b.normal()).abs() < 1e-12,
        "the two panels are genuinely non-parallel",
    );

    // ----- A -> B -> A with non-adjacent repetition -------------------------
    // Parallel panels A: x=1 and B: x=3, L=(2,2), R=(2,0).
    let para_a = panel(
        glm::vec3(1.0, 1.0, 0.0),
        glm::vec3(0.0, 2.5, 0.0),
        glm::vec3(0.0, 0.0, 0.5),
        glm::vec3(0.9, 0.9, 0.9),
    );
    let para_b = panel(
        glm::vec3(3.0, 1.0, 0.0),
        glm::vec3(0.0, 2.5, 0.0),
        glm::vec3(0.0, 0.0, 0.5),
        glm::vec3(0.8, 0.8, 0.8),
    );
    let para = MirrorTransport::new(vec![para_a, para_b], 4).unwrap();
    let para_light = glm::vec3(2.0, 2.0, 0.0);
    let para_receiver = glm::vec3(2.0, 0.0, 0.0);
    let para_all = para.connect(&empty_scene, para_receiver, para_light, 4);
    let aba = find_connection(&para_all, &[0, 1, 0]).expect("A-B-A connection");
    g.check(
        vec3_close(aba.vertices[0], glm::vec3(1.0, 5.0 / 3.0, 0.0), 1e-9),
        "A-B-A first fold at hand-derived (1, 5/3)",
    );
    g.check(
        vec3_close(aba.vertices[1], glm::vec3(3.0, 1.0, 0.0), 1e-9),
        "A-B-A middle fold at hand-derived (3, 1)",
    );
    g.check(
        vec3_close(aba.vertices[2], glm::vec3(1.0, 1.0 / 3.0, 0.0), 1e-9),
        "A-B-A last fold at hand-derived (1, 1/3)",
    );
    g.check(
        (aba.total_length - 2.0 * 10.0_f64.sqrt()).abs() < 1e-9,
        "A-B-A unfolded length is 2*sqrt(10)",
    );
    g.check(
        (aba.reflectance_product.x - 0.9 * 0.9 * 0.8).abs() < 1e-12,
        "A-B-A reflectance product is R_A^2 * R_B",
    );
    for connection in &para_all {
        for adjacent in connection.panels.windows(2) {
            g.check(
                adjacent[0] != adjacent[1],
                "consecutive duplicate panels are never enumerated",
            );
        }
    }
    let para_budget_two = para.connect(&empty_scene, para_receiver, para_light, 2);
    g.check(
        para_budget_two
            .iter()
            .all(|connection| connection.panels.len() <= 2),
        "budget of 2 excludes the three-reflection path",
    );
    g.check(
        find_connection(&para_budget_two, &[0, 1, 0]).is_none(),
        "A-B-A absent under budget 2",
    );
    let para_budget_three = para.connect(&empty_scene, para_receiver, para_light, 3);
    g.check(
        find_connection(&para_budget_three, &[0, 1, 0]).is_some(),
        "A-B-A present under budget 3",
    );

    // ----- Reverse reciprocity ----------------------------------------------
    let swapped = corner.connect(&empty_scene, corner_light, corner_receiver, 4);
    let swapped_two = find_connection(&swapped, &[1, 0])
        .expect("reversed path uses the reversed panel order");
    g.check(
        vec3_close(swapped_two.vertices[0], two.vertices[1], 1e-9)
            && vec3_close(swapped_two.vertices[1], two.vertices[0], 1e-9),
        "swapped endpoints traverse the same folds in reverse",
    );
    g.check(
        (swapped_two.total_length - two.total_length).abs() < 1e-10,
        "reciprocity preserves total path length",
    );
    let one_reversed = transport.connect(&empty_scene, light, receiver, 4);
    let one_back = find_connection(&one_reversed, &[0]).unwrap();
    g.check(
        vec3_close(one_back.vertices[0], one.vertices[0], 1e-12),
        "single-mirror fold is identical under endpoint swap",
    );

    // ----- Rigid-body transformation invariance -----------------------------
    let transform = glm::translate(&glm::identity(), &glm::vec3(5.0, -3.0, 2.0))
        * glm::rotate_z(&glm::identity(), std::f64::consts::FRAC_PI_2);
    let rotate = |v: glm::DVec3| glm::mat4_to_mat3(&transform) * v;
    let xform = |v: glm::DVec3| (transform * v.to_homogeneous()).xyz();
    let transformed_panel = panel(
        xform(mirror_a.center),
        rotate(mirror_a.edge_u),
        rotate(mirror_a.edge_v),
        mirror_a.reflectance,
    );
    let transformed_transport = MirrorTransport::new(vec![transformed_panel], 4).unwrap();
    let transformed_connections = transformed_transport.connect(
        &empty_scene,
        xform(receiver),
        xform(light),
        4,
    );
    let transformed_one = find_connection(&transformed_connections, &[0])
        .expect("transformed one-mirror connection");
    g.check(
        vec3_close(
            transformed_one.vertices[0],
            xform(one.vertices[0]),
            1e-9,
        ),
        "fold point transforms rigidly with the scene",
    );
    g.check(
        (transformed_one.total_length - one.total_length).abs() < 1e-9,
        "rigid transform preserves total path length",
    );

    g.finish();
}

#[test]
fn mirror_visibility() {
    let g = taskcheck::Group::new("mirror_visibility");

    let light = glm::vec3(0.0, 4.0, 0.0);
    let receiver = glm::vec3(0.0, 0.0, 0.0);
    let main_mirror = panel(
        glm::vec3(2.0, 2.0, 0.0),
        glm::vec3(0.0, 2.0, 0.0),
        glm::vec3(0.0, 0.0, 0.5),
        white(),
    );

    // Baseline: an unobstructed folded path exists, while the straight line
    // R->L is the vertical segment x=0.
    let clear = MirrorTransport::new(vec![main_mirror], 4).unwrap();
    g.check(
        find_connection(&clear.connect(&Scene::new(), receiver, light, 4), &[0]).is_some(),
        "unobstructed mirror connection exists",
    );

    // 1. Ordinary opaque object on a folded segment but off the straight line.
    let mut scene_blocked = Scene::new();
    scene_blocked.add(
        Object::new(
            sphere()
                .scale(&glm::vec3(0.1, 0.1, 0.1))
                .translate(&glm::vec3(1.0, 1.0, 0.0)),
        )
        .material(Material::diffuse(glm::vec3(0.2, 0.2, 0.2))),
    );
    g.check(
        clear
            .connect(&scene_blocked, receiver, light, 4)
            .is_empty(),
        "sphere on the folded segment (off the straight line) breaks the connection",
    );

    // 2. A second, non-sequence mirror panel truncates the segment.
    let blocker_panel = panel(
        glm::vec3(1.0, 1.0, 0.0),
        glm::vec3(0.0, 0.3, 0.0),
        glm::vec3(0.0, 0.0, 0.3),
        white(),
    );
    let with_blocker_panel = MirrorTransport::new(vec![main_mirror, blocker_panel], 4).unwrap();
    g.check(
        with_blocker_panel
            .connect(&Scene::new(), receiver, light, 4)
            .is_empty(),
        "another mirror panel across the segment is an occluder, not transparent",
    );

    // 3. Fold point outside the finite aperture yields no connection.
    let small_mirror = panel(
        glm::vec3(2.0, 3.0, 0.0),
        glm::vec3(0.0, 0.5, 0.0),
        glm::vec3(0.0, 0.0, 0.5),
        white(),
    );
    let small_transport = MirrorTransport::new(vec![small_mirror], 4).unwrap();
    g.check(
        small_transport
            .connect(&Scene::new(), receiver, light, 4)
            .is_empty(),
        "fold outside the finite mirror aperture is rejected",
    );

    // 4. Object behind the mirror (on the virtual straight line) does not block.
    let mut scene_behind = Scene::new();
    scene_behind.add(
        Object::new(
            sphere()
                .scale(&glm::vec3(0.1, 0.1, 0.1))
                .translate(&glm::vec3(3.0, 3.0, 0.0)),
        )
        .material(Material::diffuse(glm::vec3(0.2, 0.2, 0.2))),
    );
    g.check(
        find_connection(
            &clear.connect(&scene_behind, receiver, light, 4),
            &[0],
        )
        .is_some(),
        "occluder behind the mirror on the virtual line is not on the real path",
    );

    // 5. Object behind the light does not shade.
    let mut scene_past_light = Scene::new();
    scene_past_light.add(
        Object::new(
            sphere()
                .scale(&glm::vec3(0.1, 0.1, 0.1))
                .translate(&glm::vec3(0.0, 5.0, 0.0)),
        )
        .material(Material::diffuse(glm::vec3(0.2, 0.2, 0.2))),
    );
    g.check(
        find_connection(
            &clear.connect(&scene_past_light, receiver, light, 4),
            &[0],
        )
        .is_some(),
        "objects beyond the light do not occlude",
    );

    // 6. Two distinct, simultaneously valid one-reflection connections.
    let left_mirror = panel(
        glm::vec3(-2.0, 2.0, 0.0),
        glm::vec3(0.0, 2.0, 0.0),
        glm::vec3(0.0, 0.0, 0.5),
        white(),
    );
    let two_sided = MirrorTransport::new(vec![main_mirror, left_mirror], 4).unwrap();
    let coexist = two_sided.connect(&Scene::new(), receiver, light, 1);
    g.check(coexist.len() == 2, "two mirror connections coexist");
    let right = find_connection(&coexist, &[0]).expect("right mirror path");
    let left = find_connection(&coexist, &[1]).expect("left mirror path");
    g.check(
        vec3_close(right.vertices[0], glm::vec3(2.0, 2.0, 0.0), 1e-9)
            && vec3_close(left.vertices[0], glm::vec3(-2.0, 2.0, 0.0), 1e-9),
        "coexisting folds are at the two distinct apertures",
    );
    g.check(
        (right.total_length - left.total_length).abs() < 1e-9,
        "symmetric coexisting paths have equal total length",
    );

    // 7. Aperture boundary is interior, with finite tolerance.
    let boundary_mirror = panel(
        glm::vec3(2.0, 3.0, 0.0),
        glm::vec3(0.0, 1.0, 0.0),
        glm::vec3(0.0, 0.0, 0.5),
        white(),
    );
    let boundary_transport = MirrorTransport::new(vec![boundary_mirror], 4).unwrap();
    g.check(
        find_connection(
            &boundary_transport.connect(&Scene::new(), receiver, light, 1),
            &[0],
        )
        .is_some(),
        "fold exactly on the aperture boundary counts as inside",
    );

    g.finish();
}

/// Scene where the receiver at the origin is shadowed from its point light by
/// a cube, but a vertical mirror at x=2 can fold light around it.
fn shadowed_scene(light_position: glm::DVec3) -> (Scene, MirrorPanel) {
    let mut scene = Scene::new();
    scene.add(
        Object::new(Plane {
            normal: glm::vec3(0.0, 1.0, 0.0),
            value: 0.0,
        })
        .material(Material::diffuse(white())),
    );
    scene.add(
        Object::new(
            cube()
                .scale(&glm::vec3(0.6, 0.6, 0.6))
                .translate(&glm::vec3(0.0, 1.5, 0.0)),
        )
        .material(Material::diffuse(glm::vec3(0.3, 0.3, 0.3))),
    );
    scene.add(Light::Point(glm::vec3(80.0, 80.0, 80.0), light_position));
    let mirror = panel(
        glm::vec3(2.0, 2.5, 0.0),
        glm::vec3(0.0, 3.0, 0.0),
        glm::vec3(0.0, 0.0, 0.5),
        white(),
    );
    (scene, mirror)
}

fn receiver_ray() -> Ray {
    // Origin below the occluder's bottom face (y = 1.2), so the ray reaches
    // the ground receiver at the origin without hitting the cube first.
    Ray {
        origin: glm::vec3(0.0, 0.5, 0.0),
        dir: glm::vec3(0.0, -1.0, 0.0),
    }
}

#[test]
fn mirror_render() {
    let g = taskcheck::Group::new("mirror_render");

    // ----- Colored reflectance and total-path-length attenuation ------------
    let tint = glm::vec3(0.9, 0.5, 0.2);
    let (scene_four, mirror_four) = shadowed_scene(glm::vec3(0.0, 4.0, 0.0));
    let tinted_mirror = panel(
        mirror_four.center,
        mirror_four.edge_u,
        mirror_four.edge_v,
        tint,
    );
    let transport_four = MirrorTransport::new(vec![tinted_mirror], 4).unwrap();
    let renderer_four = Renderer::new(&scene_four, Camera::default())
        .mirror_transport(&transport_four)
        // One bounce funds the mirror connection but leaves no indirect term,
        // so the traced color is exactly the analytic connection contribution.
        .max_bounces(1);
    let traced_four = renderer_four.trace_ray_seeded(receiver_ray(), 11);

    // Independent expectation using the receiver's own BSDF and forward cosine.
    let receiver_material = Material::diffuse(white());
    let normal = glm::vec3(0.0, 1.0, 0.0);
    let wo = glm::vec3(0.0, 1.0, 0.0);
    let wi_four = glm::vec3(2.0, 2.0, 0.0).normalize();
    let f_four = receiver_material.bsdf(&normal, &wo, &wi_four);
    let cos_four = wi_four.dot(&normal);
    let length_sq_four = 32.0; // (2*sqrt(8))^2
    let expected_four =
        f_four.component_mul(&tint) * (80.0 / length_sq_four) * cos_four;
    g.check(
        (traced_four - expected_four).abs().max() < 1e-8,
        "rendered mirror contribution matches I/L^2 * R * BSDF * cosine",
    );
    g.check(
        traced_four.x > traced_four.y && traced_four.y > traced_four.z,
        "colored mirror reflectance tints the rendered path",
    );
    g.check(
        (traced_four.x / traced_four.z - tint.x / tint.z).abs() < 1e-10,
        "channel ratio equals the mirror reflectance ratio",
    );

    // Move the light to (0, 6): fold becomes (2, 3), total length 2*sqrt(13).
    let (scene_six, mirror_six) = shadowed_scene(glm::vec3(0.0, 6.0, 0.0));
    let tinted_six = panel(
        mirror_six.center,
        mirror_six.edge_u,
        mirror_six.edge_v,
        tint,
    );
    let transport_six = MirrorTransport::new(vec![tinted_six], 4).unwrap();
    let traced_six = Renderer::new(&scene_six, Camera::default())
        .mirror_transport(&transport_six)
        .max_bounces(1)
        .trace_ray_seeded(receiver_ray(), 11);
    let wi_six = glm::vec3(2.0, 3.0, 0.0).normalize();
    let f_six = receiver_material.bsdf(&normal, &wo, &wi_six);
    let cos_six = wi_six.dot(&normal);
    let length_sq_six = 52.0; // (2*sqrt(13))^2
    let expected_six = f_six.component_mul(&tint) * (80.0 / length_sq_six) * cos_six;
    g.check(
        (traced_six - expected_six).abs().max() < 1e-8,
        "longer unfolded path attenuates by the new total length squared",
    );
    g.check(
        traced_six.x < traced_four.x,
        "the longer optical path is strictly dimmer",
    );

    // ----- Straight-line shadow, folded-path illumination -------------------
    let (dark_scene, dark_mirror) = shadowed_scene(glm::vec3(0.0, 4.0, 0.0));
    let white_transport = MirrorTransport::new(vec![dark_mirror], 4).unwrap();
    let without_mirror = Renderer::new(&dark_scene, Camera::default()).max_bounces(1);
    let with_mirror = Renderer::new(&dark_scene, Camera::default())
        .mirror_transport(&white_transport)
        .max_bounces(1);
    let mut disabled_sum = glm::vec3(0.0, 0.0, 0.0);
    for seed in [1, 2, 3] {
        disabled_sum += without_mirror.trace_ray_seeded(receiver_ray(), seed);
    }
    g.check(
        disabled_sum.abs().max() == 0.0,
        "backlit receiver stays black without mirror transport",
    );
    let enabled = with_mirror.trace_ray_seeded(receiver_ray(), 1);
    g.check(
        enabled.abs().max() > 0.01,
        "the folded mirror path lights the shadowed receiver",
    );

    // ----- Eye path strikes the mirror before the background surface --------
    let mut eye_scene = Scene::new();
    eye_scene.environment = Environment::Color(glm::vec3(1.0, 0.0, 0.0));
    eye_scene.add(
        Object::new(Plane {
            normal: glm::vec3(0.0, 0.0, 1.0),
            value: -2.0,
        })
        .material(Material::diffuse(glm::vec3(0.1, 0.1, 0.1))),
    );
    let eye_mirror = panel(
        glm::vec3(0.0, 0.0, 0.0),
        glm::vec3(1.0, 0.0, 0.0),
        glm::vec3(0.0, 1.0, 0.0),
        glm::vec3(0.8, 0.8, 0.8),
    );
    let eye_transport = MirrorTransport::new(vec![eye_mirror], 4).unwrap();
    let eye_ray = Ray {
        origin: glm::vec3(0.0, 0.0, 3.0),
        dir: glm::vec3(0.0, 0.0, -1.0),
    };
    let reflected = Renderer::new(&eye_scene, Camera::default())
        .mirror_transport(&eye_transport)
        .max_bounces(2)
        .trace_ray_seeded(eye_ray, 7);
    g.check(
        (reflected - glm::vec3(0.8, 0.0, 0.0)).abs().max() < 1e-9,
        "eye ray reflects off the nearest mirror and tints the environment",
    );
    let passed_through = Renderer::new(&eye_scene, Camera::default())
        .max_bounces(2)
        .trace_ray_seeded(eye_ray, 7);
    g.check(
        passed_through.abs().max() == 0.0,
        "without mirrors the eye ray reaches the dark background",
    );

    // ----- Bounce budget boundary -------------------------------------------
    let no_budget = Renderer::new(&dark_scene, Camera::default())
        .mirror_transport(&white_transport)
        .max_bounces(0)
        .trace_ray_seeded(receiver_ray(), 5);
    g.check(
        no_budget.abs().max() == 0.0,
        "zero remaining bounces cannot fund a mirror connection",
    );
    let one_budget = Renderer::new(&dark_scene, Camera::default())
        .mirror_transport(&white_transport)
        .max_bounces(1)
        .trace_ray_seeded(receiver_ray(), 5);
    g.check(
        one_budget.abs().max() > 0.01,
        "one remaining bounce funds the one-reflection connection",
    );

    // ----- Fixed seeds: disabling the option preserves paths bit-for-bit ----
    let mut parity_scene = Scene::new();
    parity_scene.add(
        Object::new(Plane {
            normal: glm::vec3(0.0, 1.0, 0.0),
            value: 0.0,
        })
        .material(Material::diffuse(glm::vec3(0.6, 0.7, 0.8))),
    );
    parity_scene.add(Light::Point(
        glm::vec3(5.0, 5.0, 5.0),
        glm::vec3(2.0, 3.0, 1.0),
    ));
    let probe_rays = [
        Ray {
            origin: glm::vec3(0.0, 1.0, 0.0),
            dir: glm::vec3(0.0, -1.0, 0.0),
        },
        Ray {
            origin: glm::vec3(1.0, 0.5, 1.0),
            dir: glm::vec3(-0.3, -0.8, -0.5).normalize(),
        },
    ];
    let empty_transport = MirrorTransport::new(vec![], 4).unwrap();
    let far_panel = panel(
        glm::vec3(50.0, 50.0, 50.0),
        glm::vec3(1.0, 0.0, 0.0),
        glm::vec3(0.0, 1.0, 0.0),
        white(),
    );
    let far_transport = MirrorTransport::new(vec![far_panel], 4).unwrap();
    for (ray_index, ray) in probe_rays.iter().enumerate() {
        for seed in [1, 42, 999] {
            let plain = Renderer::new(&parity_scene, Camera::default())
                .max_bounces(3)
                .trace_ray_seeded(*ray, seed);
            let empty = Renderer::new(&parity_scene, Camera::default())
                .mirror_transport(&empty_transport)
                .max_bounces(3)
                .trace_ray_seeded(*ray, seed);
            let far = Renderer::new(&parity_scene, Camera::default())
                .mirror_transport(&far_transport)
                .max_bounces(3)
                .trace_ray_seeded(*ray, seed);
            g.check(
                plain == empty && plain == far,
                "fixed-seed path is unchanged when mirrors are absent or unused",
            );
            g.check(
                plain.iter().all(|component| component.is_finite()),
                &format!("probe ray {} returns finite linear color", ray_index),
            );
        }
    }

    g.finish();
}
