//! Deterministic checks for finite planar mirror transport.
//!
//! Each of the three test groups is recorded through `taskcheck::Group`, so every
//! assertion is counted and a group without checks fails. All reference geometry in
//! this file is recomputed independently (2D cross sections and hand-written virtual
//! image unfolding); the library solver is only used as the system under test.

#[path = "../../task_support/check.rs"]
mod taskcheck;

use rpt::*;

const ABS_TOL: f64 = 1e-8;

fn vec_close(a: glm::DVec3, b: glm::DVec3, tol: f64) -> bool {
    (a - b).magnitude() <= tol
}

fn panel_normal(panel: &MirrorPanel) -> glm::DVec3 {
    panel.edge_u.cross(&panel.edge_v).normalize()
}

/// Independently reflect a point across a panel plane, without calling library code.
fn independent_reflect(panel: MirrorPanel, point: glm::DVec3) -> glm::DVec3 {
    let n = panel_normal(&panel);
    let d = (point - panel.center).dot(&n);
    point - 2.0 * d * n
}

/// Hand-written unfolding for a fixed panel sequence. Returns the expected fold
/// vertices or `None` when an independent plane/line intersection is degenerate.
fn independent_fold(
    panels: &[MirrorPanel],
    light: glm::DVec3,
    receiver: glm::DVec3,
    sequence: &[usize],
) -> Option<Vec<glm::DVec3>> {
    let mut image = light;
    for &index in sequence {
        image = independent_reflect(panels[index], image);
    }
    let mut vertices = Vec::new();
    let mut origin = receiver;
    let mut target = image;
    for step in (0..sequence.len()).rev() {
        let panel = panels[sequence[step]];
        let n = panel_normal(&panel);
        let displacement = target - origin;
        let denom = n.dot(&displacement);
        if denom.abs() <= 1e-12 * displacement.magnitude() {
            return None;
        }
        let t = (panel.center - origin).dot(&n) / denom;
        if t <= 0.0 || t >= 1.0 {
            return None;
        }
        vertices.push(origin + t * displacement);
        origin = vertices[vertices.len() - 1];
        // Recompute the virtual image for this depth so the next fold uses it.
        let mut partial = light;
        for &index in &sequence[..step] {
            partial = independent_reflect(panels[index], partial);
        }
        target = partial;
    }
    vertices.reverse();
    Some(vertices)
}

/// Check the law of reflection at every fold of a solved path, using independent math.
fn reflection_law_holds(panels: &[MirrorPanel], light: glm::DVec3, receiver: glm::DVec3, path: &MirrorPath) -> bool {
    let mut chain = vec![light];
    chain.extend(path.points.iter().copied());
    chain.push(receiver);
    for (fold, &panel_index) in path.panels.iter().enumerate() {
        let n = panel_normal(&panels[panel_index]);
        let incoming = (chain[fold + 1] - chain[fold]).normalize();
        let outgoing = (chain[fold + 2] - chain[fold + 1]).normalize();
        let reflected = incoming - 2.0 * incoming.dot(&n) * n;
        if (reflected - outgoing).magnitude() > 1e-7 {
            return false;
        }
        if (chain[fold + 2] - chain[fold + 1]).magnitude() <= 1e-9 {
            return false;
        }
    }
    true
}

fn vertical_panel(center_x: f64, center_z: f64, half_y: f64, half_z: f64, reflectance: glm::DVec3) -> MirrorPanel {
    MirrorPanel {
        center: glm::vec3(center_x, 0.0, center_z),
        edge_u: glm::vec3(0.0, half_y, 0.0),
        edge_v: glm::vec3(0.0, 0.0, half_z),
        reflectance,
    }
}

fn ground() -> Object {
    Object::new(Plane {
        normal: glm::vec3(0.0, 0.0, 1.0),
        value: 0.0,
    })
    .material(Material::diffuse(glm::vec3(1.0, 1.0, 1.0)))
}

#[test]
fn mirror_geometry() {
    let g = taskcheck::Group::new("mirror_geometry");

    // ---- Construction validation ----
    let good = vertical_panel(0.0, 1.0, 2.0, 2.0, glm::vec3(0.8, 0.7, 0.6));
    g.check(MirrorTransport::new(vec![good; 9], 1).is_err(), "more than 8 panels rejected");
    g.check(MirrorTransport::new(vec![good], 5).is_err(), "max_reflections above 4 rejected");
    g.check(MirrorTransport::new(vec![good], 0).is_ok(), "zero reflection budget accepted");
    let zero_edge = MirrorPanel {
        center: glm::vec3(0.0, 0.0, 0.0),
        edge_u: glm::vec3(0.0, 0.0, 0.0),
        edge_v: glm::vec3(0.0, 1.0, 0.0),
        reflectance: glm::vec3(1.0, 1.0, 1.0),
    };
    g.check(MirrorTransport::new(vec![zero_edge], 1).is_err(), "zero half-edge rejected");
    let skew = MirrorPanel {
        center: glm::vec3(0.0, 0.0, 0.0),
        edge_u: glm::vec3(1.0, 0.0, 0.0),
        edge_v: glm::vec3(1.0, 1.0, 0.0),
        reflectance: glm::vec3(1.0, 1.0, 1.0),
    };
    g.check(MirrorTransport::new(vec![skew], 1).is_err(), "non-orthogonal half-edges rejected");
    let bad_color = MirrorPanel {
        center: glm::vec3(0.0, 0.0, 0.0),
        edge_u: glm::vec3(1.0, 0.0, 0.0),
        edge_v: glm::vec3(0.0, 1.0, 0.0),
        reflectance: glm::vec3(0.5, -0.1, 1.2),
    };
    g.check(MirrorTransport::new(vec![bad_color], 1).is_err(), "out-of-range reflectance rejected");
    let infinite = MirrorPanel {
        center: glm::vec3(f64::NAN, 0.0, 0.0),
        edge_u: glm::vec3(1.0, 0.0, 0.0),
        edge_v: glm::vec3(0.0, 1.0, 0.0),
        reflectance: glm::vec3(1.0, 1.0, 1.0),
    };
    g.check(MirrorTransport::new(vec![infinite], 1).is_err(), "non-finite center rejected");

    let empty_scene = Scene::new();

    // ---- One mirror against a 2D cross-section reference ----
    // Panel is the x = 0 line in the xz plane; light (2, 0, 2), receiver (2, 0, 0).
    // The virtual image is (-2, 0, 2); the receiver->image segment crosses x = 0 at
    // its midpoint, so the fold vertex is exactly (0, 0, 1) and length is 2 sqrt(5).
    let panel = vertical_panel(0.0, 1.0, 2.0, 2.0, glm::vec3(0.8, 0.7, 0.6));
    let transport = MirrorTransport::new(vec![panel], 4).unwrap();
    let light = glm::vec3(2.0, 0.0, 2.0);
    let receiver = glm::vec3(2.0, 0.0, 0.0);
    let paths = transport.connect(&empty_scene, &light, &receiver, 4);
    g.check(paths.len() == 1, "one-mirror configuration yields one path");
    let path = &paths[0];
    g.check(path.panels == vec![0], "one-mirror path records panel index 0");
    g.check(
        vec_close(path.points[0], glm::vec3(0.0, 0.0, 1.0), ABS_TOL),
        "fold vertex matches 2D cross-section (0,0,1)",
    );
    g.check(
        (path.total_length - 2.0 * 5.0_f64.sqrt()).abs() < ABS_TOL,
        "total length matches 2 sqrt(5)",
    );
    g.check(
        vec_close(path.reflectance, glm::vec3(0.8, 0.7, 0.6), ABS_TOL),
        "reflectance product is the panel reflectance",
    );
    g.check(
        reflection_law_holds(transport.panels(), light, receiver, path),
        "one-mirror path obeys the reflection law",
    );
    // Straight receiver->virtual-image distance must equal the folded length.
    let image = independent_reflect(panel, light);
    g.check(
        ((image - receiver).magnitude() - path.total_length).abs() < ABS_TOL,
        "folded length equals straight virtual-image distance",
    );

    // ---- Aperture boundary counts as interior ----
    // Receiver (2, 0, -2) places the fold at (0, 0, 0), the bottom edge of the panel.
    let boundary_paths = transport.connect(&empty_scene, &light, &glm::vec3(2.0, 0.0, -2.0), 1);
    g.check(boundary_paths.len() == 1, "fold on aperture boundary is accepted");
    g.check(
        vec_close(boundary_paths[0].points[0], glm::vec3(0.0, 0.0, 0.0), ABS_TOL),
        "boundary fold vertex is exactly on the edge",
    );

    // ---- Degenerate configurations contribute nothing ----
    // Light lying on the mirror plane collapses the first segment to zero length.
    g.check(
        transport
            .connect(&empty_scene, &glm::vec3(0.0, 0.0, 1.0), &receiver, 1)
            .is_empty(),
        "light on mirror plane gives no path",
    );
    // Coplanar light and receiver: unfolded line lies in the panel plane.
    g.check(
        transport
            .connect(
                &empty_scene,
                &glm::vec3(0.0, 1.0, 1.0),
                &glm::vec3(0.0, -1.0, 1.0),
                1,
            )
            .is_empty(),
        "coplanar unfolded line gives no path",
    );
    // Receiver coincides with the virtual image: zero-length unfolded line.
    g.check(
        transport.connect(&empty_scene, &light, &image, 1).is_empty(),
        "receiver at virtual image gives no path",
    );
    // Vertex outside a shrunken aperture is rejected.
    let small = MirrorTransport::new(
        vec![vertical_panel(0.0, 3.0, 0.2, 0.2, glm::vec3(1.0, 1.0, 1.0))],
        1,
    )
    .unwrap();
    g.check(
        small.connect(&empty_scene, &light, &receiver, 1).is_empty(),
        "fold outside finite aperture gives no path",
    );

    // ---- Non-parallel (perpendicular) two mirrors ----
    let panel_a = vertical_panel(0.0, 1.5, 3.0, 1.5, glm::vec3(0.9, 0.8, 0.7));
    let panel_b = MirrorPanel {
        center: glm::vec3(0.0, 0.0, 1.5),
        edge_u: glm::vec3(3.0, 0.0, 0.0),
        edge_v: glm::vec3(0.0, 0.0, 1.5),
        reflectance: glm::vec3(0.6, 0.7, 0.8),
    };
    let two = MirrorTransport::new(vec![panel_a, panel_b], 4).unwrap();
    let light2 = glm::vec3(2.0, 2.0, 1.0);
    let receiver2 = glm::vec3(2.0, 1.0, 2.0);
    let two_paths = two.connect(&empty_scene, &light2, &receiver2, 4);
    let ab = two_paths.iter().find(|p| p.panels == vec![0, 1]);
    g.check(ab.is_some(), "non-parallel mirrors admit an A->B sequence");
    if let Some(ab) = ab {
        let expected = independent_fold(two.panels(), light2, receiver2, &[0, 1]).unwrap();
        g.check(
            ab.points
                .iter()
                .zip(expected.iter())
                .all(|(a, b)| vec_close(*a, *b, 1e-7)),
            "A->B vertices match independent unfolding",
        );
        // Hard-coded 3D cross-section values for this configuration.
        g.check(
            vec_close(ab.points[0], glm::vec3(0.0, 0.5, 1.5), 1e-7)
                && vec_close(ab.points[1], glm::vec3(2.0 / 3.0, 0.0, 5.0 / 3.0), 1e-7),
            "A->B vertices match analytic coordinates",
        );
        g.check(
            reflection_law_holds(two.panels(), light2, receiver2, ab),
            "A->B path obeys reflection at both folds",
        );
        let expected_refl = panel_a.reflectance.component_mul(&panel_b.reflectance);
        g.check(vec_close(ab.reflectance, expected_refl, ABS_TOL), "A->B reflectance product");
    }
    // No sequence ever contains two consecutive identical panel indices.
    g.check(
        two_paths
            .iter()
            .all(|p| p.panels.windows(2).all(|w| w[0] != w[1])),
        "consecutive same-panel reflections are omitted",
    );
    g.check(
        two_paths.iter().all(|p| reflection_law_holds(two.panels(), light2, receiver2, p)),
        "all two-mirror paths obey the reflection law",
    );

    // ---- A -> B -> A between parallel mirrors, against independent unfolding ----
    let par_a = vertical_panel(-2.0, 0.0, 4.0, 4.0, glm::vec3(0.9, 0.9, 0.9));
    let par_b = vertical_panel(2.0, 0.0, 4.0, 4.0, glm::vec3(0.8, 0.8, 0.8));
    let parallel = MirrorTransport::new(vec![par_a, par_b], 4).unwrap();
    let light3 = glm::vec3(0.5, 0.0, 1.5);
    let receiver3 = glm::vec3(-0.5, 0.0, -1.5);
    let par_paths = parallel.connect(&empty_scene, &light3, &receiver3, 4);
    let aba = par_paths.iter().find(|p| p.panels == vec![0, 1, 0]);
    g.check(aba.is_some(), "parallel mirrors admit a non-adjacent A->B->A repeat");
    if let Some(aba) = aba {
        let expected = independent_fold(parallel.panels(), light3, receiver3, &[0, 1, 0]).unwrap();
        g.check(
            aba.points
                .iter()
                .zip(expected.iter())
                .all(|(a, b)| vec_close(*a, *b, 1e-7)),
            "A->B->A vertices match independent unfolding",
        );
        g.check(
            vec_close(aba.points[0], glm::vec3(-2.0, 0.0, 0.875), 1e-7)
                && vec_close(aba.points[1], glm::vec3(2.0, 0.0, -0.125), 1e-7)
                && vec_close(aba.points[2], glm::vec3(-2.0, 0.0, -1.125), 1e-7),
            "A->B->A vertices match analytic coordinates",
        );
        g.check(
            reflection_law_holds(parallel.panels(), light3, receiver3, aba),
            "A->B->A obeys reflection at all three folds",
        );
        // A->B->A must not be simplified away like a consecutive repeat.
        g.check(aba.panels.len() == 3, "non-adjacent repeat keeps all three reflections");
    }

    // ---- Reverse reciprocity ----
    let reverse = parallel.connect(&empty_scene, &receiver3, &light3, 4);
    g.check(reverse.len() == par_paths.len(), "reciprocal scene has the same path count");
    g.check(
        par_paths.iter().all(|p| {
            let wanted: Vec<usize> = p.panels.iter().rev().copied().collect();
            reverse.iter().any(|q| {
                q.panels == wanted
                    && (q.total_length - p.total_length).abs() < 1e-8
                    && q.points
                        .iter()
                        .zip(p.points.iter().rev())
                        .all(|(a, b)| vec_close(*a, *b, 1e-7))
            })
        }),
        "reversing light and receiver reverses every path",
    );

    // ---- Rigid transform invariance ----
    let angle = 0.7;
    let rotation = glm::rotate_z(&glm::identity(), angle);
    let shift = glm::vec3(1.3, -0.7, 0.4);
    let xform = |v: glm::DVec3| (rotation * v.to_homogeneous()).xyz() + shift;
    let transformed_panels: Vec<_> = parallel
        .panels()
        .iter()
        .map(|p| MirrorPanel {
            center: xform(p.center),
            edge_u: (rotation * p.edge_u.to_homogeneous()).xyz(),
            edge_v: (rotation * p.edge_v.to_homogeneous()).xyz(),
            reflectance: p.reflectance,
        })
        .collect();
    let transformed = MirrorTransport::new(transformed_panels, 4).unwrap();
    let t_paths = transformed.connect(&empty_scene, &xform(light3), &xform(receiver3), 4);
    g.check(t_paths.len() == par_paths.len(), "rigid transform preserves path count");
    g.check(
        par_paths.iter().all(|p| {
            t_paths.iter().any(|q| {
                q.panels == p.panels
                    && (q.total_length - p.total_length).abs() < 1e-6
                    && q.points
                        .iter()
                        .zip(p.points.iter())
                        .all(|(a, b)| vec_close(*a, xform(*b), 1e-6))
            })
        }),
        "rigid transform maps every fold vertex rigidly",
    );

    g.finish();
}

#[test]
fn mirror_visibility() {
    let g = taskcheck::Group::new("mirror_visibility");

    let panel = vertical_panel(0.0, 1.0, 2.0, 2.0, glm::vec3(1.0, 1.0, 1.0));
    let light = glm::vec3(2.0, 0.0, 2.0);
    let receiver = glm::vec3(2.0, 0.0, 0.0);
    // Fold vertex (0, 0, 1); both folded segments live at x in [0, 2], while the
    // straight light->receiver line is entirely at x = 2.
    let transport = MirrorTransport::new(vec![panel], 2).unwrap();

    // Baseline: unoccluded folded path exists.
    g.check(
        transport.connect(&Scene::new(), &light, &receiver, 2).len() == 1,
        "unoccluded folded connection exists",
    );

    // ---- Ordinary object on a folded segment but off the straight line ----
    let vertex = glm::vec3(0.0, 0.0, 1.0);
    let mid_light = (light + vertex) * 0.5; // (1, 0, 1.5), x = 1 != 2
    let mut scene = Scene::new();
    scene.add(
        Object::new(
            sphere()
                .scale(&glm::vec3(0.1, 0.1, 0.1))
                .translate(&mid_light),
        )
        .material(Material::diffuse(glm::vec3(0.2, 0.2, 0.2))),
    );
    g.check(
        transport.connect(&scene, &light, &receiver, 2).is_empty(),
        "object on light->mirror segment blocks the connection",
    );

    // Same test on the mirror->receiver segment (behind the mirror from the light).
    let mid_receiver = (vertex + receiver) * 0.5; // (1, 0, 0.5)
    let mut scene2 = Scene::new();
    scene2.add(
        Object::new(
            sphere()
                .scale(&glm::vec3(0.1, 0.1, 0.1))
                .translate(&mid_receiver),
        )
        .material(Material::diffuse(glm::vec3(0.2, 0.2, 0.2))),
    );
    g.check(
        transport.connect(&scene2, &light, &receiver, 2).is_empty(),
        "object behind the mirror blocks the receiver-side segment",
    );

    // ---- An object past the light does not occlude ----
    let beyond_light = light + (light - vertex).normalize() * 0.3;
    let mut scene3 = Scene::new();
    scene3.add(
        Object::new(
            sphere()
                .scale(&glm::vec3(0.05, 0.05, 0.05))
                .translate(&beyond_light),
        )
        .material(Material::diffuse(glm::vec3(0.2, 0.2, 0.2))),
    );
    g.check(
        transport.connect(&scene3, &light, &receiver, 2).len() == 1,
        "objects behind the point light do not occlude",
    );

    // ---- The receiver's own ground plane is an endpoint self-intersection ----
    let mut scene4 = Scene::new();
    scene4.add(ground());
    g.check(
        transport.connect(&scene4, &light, &receiver, 2).len() == 1,
        "receiver surface touching the segment endpoint is ignored as self-intersection",
    );

    // ---- A non-sequence mirror panel crossing a segment truncates it ----
    // The segment crosses x = 1 at (1, 0, 1.5). The blocker is a small panel around
    // that point, but too short in z for its own fold vertex (which would land at
    // z = 1, outside its z range [1.2, 1.8]), so it can only occlude, not reflect.
    let blocker = MirrorPanel {
        center: glm::vec3(1.0, 0.0, 1.5),
        edge_u: glm::vec3(0.0, 0.5, 0.0),
        edge_v: glm::vec3(0.0, 0.0, 0.3),
        reflectance: glm::vec3(0.5, 0.5, 0.5),
    };
    let with_blocker = MirrorTransport::new(vec![panel, blocker], 2).unwrap();
    g.check(
        with_blocker.connect(&Scene::new(), &light, &receiver, 2).is_empty(),
        "a mirror panel outside the sequence blocks the segment (mirrors are not transparent)",
    );
    // A panel large enough to carry the fold vertex is itself a valid mirror.
    let blocker_only = MirrorTransport::new(
        vec![MirrorPanel {
            center: glm::vec3(1.0, 0.0, 1.5),
            edge_u: glm::vec3(0.0, 0.5, 0.0),
            edge_v: glm::vec3(0.0, 0.0, 0.5),
            reflectance: glm::vec3(0.5, 0.5, 0.5),
        }],
        1,
    )
    .unwrap();
    let via_blocker = blocker_only.connect(&Scene::new(), &light, &receiver, 1);
    g.check(
        via_blocker.len() == 1 && via_blocker[0].panels == vec![0],
        "the blocking panel is itself a valid mirror when it is in the sequence",
    );

    // ---- Aperture-outside fold is rejected even with no occluders ----
    let misplaced = MirrorTransport::new(
        vec![vertical_panel(0.0, 3.0, 0.25, 0.25, glm::vec3(1.0, 1.0, 1.0))],
        1,
    )
    .unwrap();
    g.check(
        misplaced.connect(&Scene::new(), &light, &receiver, 1).is_empty(),
        "fold landing outside the finite mirror mouth contributes nothing",
    );

    // ---- Two distinct valid connections coexist ----
    let par_a = vertical_panel(-2.0, 0.0, 4.0, 4.0, glm::vec3(0.9, 0.7, 0.5));
    let par_b = vertical_panel(2.0, 0.0, 4.0, 4.0, glm::vec3(0.5, 0.7, 0.9));
    let parallel = MirrorTransport::new(vec![par_a, par_b], 4).unwrap();
    let pl = glm::vec3(0.0, 0.0, 1.0);
    let pr = glm::vec3(0.0, 0.0, -1.0);
    let coexist = parallel.connect(&Scene::new(), &pl, &pr, 4);
    let via_a = coexist.iter().any(|p| p.panels == vec![0]);
    let via_b = coexist.iter().any(|p| p.panels == vec![1]);
    g.check(via_a && via_b, "two distinct one-bounce connections coexist");
    g.check(
        coexist.iter().filter(|p| p.panels.len() == 1).count() == 2,
        "exactly two one-bounce paths through the two parallel mirrors",
    );
    g.check(
        coexist
            .iter()
            .find(|p| p.panels == vec![0])
            .unwrap()
            .reflectance
            != coexist
                .iter()
                .find(|p| p.panels == vec![1])
                .unwrap()
                .reflectance,
        "coexisting paths carry their own panel reflectance",
    );

    g.finish();
}

#[test]
fn mirror_render() {
    let g = taskcheck::Group::new("mirror_render");

    // Receiver ground point P with its straight-line light path blocked by a sphere.
    let light_pos = glm::vec3(2.0, 0.0, 2.0);
    let light_color = glm::vec3(10.0, 10.0, 10.0);
    let receiver = glm::vec3(2.0, 0.0, 0.0);
    let eye = glm::vec3(2.0, -2.0, 2.0);
    let ray = Ray {
        origin: eye,
        dir: (receiver - eye).normalize(),
    };

    let build_scene = || {
        let mut scene = Scene::new();
        scene.add(ground());
        scene.add(Light::Point(light_color, light_pos));
        scene.add(
            Object::new(
                sphere()
                    .scale(&glm::vec3(0.15, 0.15, 0.15))
                    .translate(&glm::vec3(2.0, 0.0, 1.0)),
            )
            .material(Material::diffuse(glm::vec3(0.1, 0.1, 0.1))),
        );
        scene
    };
    let scene = build_scene();

    let colored = vertical_panel(0.0, 2.0, 4.0, 2.0, glm::vec3(0.9, 0.5, 0.2));
    let white = vertical_panel(0.0, 2.0, 4.0, 2.0, glm::vec3(1.0, 1.0, 1.0));
    let transport = MirrorTransport::new(vec![colored], 2).unwrap();
    let white_transport = MirrorTransport::new(vec![white], 2).unwrap();

    let renderer_off = Renderer::new(&scene, Camera::default()).max_bounces(1);
    let renderer_on = Renderer::new(&scene, Camera::default())
        .mirror_transport(&transport)
        .max_bounces(1);
    let renderer_white = Renderer::new(&scene, Camera::default())
        .mirror_transport(&white_transport)
        .max_bounces(1);

    let color_off = renderer_off.trace_ray_seeded(ray, 42);
    let color_on = renderer_on.trace_ray_seeded(ray, 42);
    let color_white = renderer_white.trace_ray_seeded(ray, 42);

    // Straight line is backlit: the disabled renderer sees only black at this pixel.
    g.check(
        color_off.magnitude() < ABS_TOL,
        "direct path blocked: baseline pixel is black",
    );
    g.check(
        color_on.magnitude() > 0.01,
        "folded specular path lights the backlit pixel",
    );

    // Colored reflectance multiplies each channel independently.
    let delta_colored = color_on - color_off;
    let delta_white = color_white - color_off;
    let ratios = glm::vec3(
        delta_colored.x / delta_white.x,
        delta_colored.y / delta_white.y,
        delta_colored.z / delta_white.z,
    );
    g.check(
        vec_close(ratios, glm::vec3(0.9, 0.5, 0.2), 1e-10),
        "folded contribution carries per-channel RGB reflectance",
    );

    // Exact match with the analytic folded term: BSDF * (E / L^2) * reflectance * cosine,
    // attenuated by the TOTAL path length rather than per-segment distances.
    let path = &transport.connect(&scene, &light_pos, &receiver, 2)[0];
    let material = Material::diffuse(glm::vec3(1.0, 1.0, 1.0));
    let n = glm::vec3(0.0, 0.0, 1.0);
    let wo = -ray.dir;
    let wi = (path.points[0] - receiver).normalize();
    let expected = material
        .bsdf(&n, &wo, &wi)
        .component_mul(&(light_color / path.total_length.powi(2)))
        .component_mul(&colored.reflectance)
        * wi.dot(&n);
    g.check(
        vec_close(delta_colored, expected, 1e-12),
        "rendered folded term matches total-length analytic contribution",
    );

    // Total-path-length attenuation law: move the light farther along the same
    // reflected ray so the fold vertex and receiver-side direction are unchanged and
    // only the total polyline length changes.
    // L1 = (2,0,2) gives vertex (0,0,1) and length 2 sqrt(5); L2 = (4,0,3) keeps the
    // same vertex and length 3 sqrt(5), so the intensity ratio must be (2/3)^2 = 4/9.
    let light2_pos = glm::vec3(4.0, 0.0, 3.0);
    let mut scene_len = Scene::new();
    scene_len.add(ground());
    scene_len.add(Light::Point(light_color, light2_pos));
    let straight_dir = (light2_pos - receiver).normalize();
    scene_len.add(
        Object::new(
            sphere()
                .scale(&glm::vec3(0.2, 0.2, 0.2))
                .translate(&(receiver + straight_dir * 0.5)),
        )
        .material(Material::diffuse(glm::vec3(0.1, 0.1, 0.1))),
    );
    let len_transport = MirrorTransport::new(
        vec![vertical_panel(0.0, 2.0, 4.0, 2.0, glm::vec3(1.0, 1.0, 1.0))],
        2,
    )
    .unwrap();
    let p1 = transport.connect(&scene, &light_pos, &receiver, 2)[0].total_length;
    let len_paths = len_transport.connect(&scene_len, &light2_pos, &receiver, 2);
    g.check(len_paths.len() == 1, "longer folded configuration still connects");
    let p2 = len_paths[0].total_length;
    g.check(
        vec_close(len_paths[0].points[0], glm::vec3(0.0, 0.0, 1.0), 1e-8),
        "the longer configuration shares the same fold vertex and receiver direction",
    );
    let c1 = renderer_white.trace_ray_seeded(ray, 42);
    let c2 = Renderer::new(&scene_len, Camera::default())
        .mirror_transport(&len_transport)
        .max_bounces(1)
        .trace_ray_seeded(ray, 42);
    let rendered_ratio = c2.x / c1.x;
    let analytic_ratio = (p1 / p2).powi(2);
    g.check(
        (rendered_ratio - analytic_ratio).abs() < 1e-10 && (analytic_ratio - 4.0 / 9.0).abs() < 1e-10,
        "folded intensity falls off with the square of total path length",
    );

    // The folded term is deterministic and independent of the RNG seed.
    let seed_a = renderer_on.trace_ray_seeded(ray, 7) - renderer_off.trace_ray_seeded(ray, 7);
    let seed_b = renderer_on.trace_ray_seeded(ray, 99) - renderer_off.trace_ray_seeded(ray, 99);
    g.check(
        vec_close(seed_a, seed_b, 1e-12),
        "mirror connections are seed-independent",
    );

    // ---- Eye path: nearest-hit mirror wins over a farther ordinary surface ----
    let build_eye_scene = || {
        let mut scene = Scene::new();
        scene.add(ground());
        scene.add(Light::Point(glm::vec3(20.0, 20.0, 20.0), glm::vec3(-3.0, 0.0, 3.0)));
        scene
    };
    let eye_scene = build_eye_scene();
    let eye_panel = vertical_panel(0.0, 1.0, 3.0, 1.0, glm::vec3(0.8, 0.6, 0.4));
    let eye_transport = MirrorTransport::new(vec![eye_panel], 1).unwrap();
    let mirror_ray = Ray {
        origin: glm::vec3(3.0, 0.0, 2.0),
        dir: (glm::vec3(0.0, 0.0, 1.0) - glm::vec3(3.0, 0.0, 2.0)).normalize(),
    };
    let eye_off = Renderer::new(&eye_scene, Camera::default())
        .max_bounces(1)
        .trace_ray_seeded(mirror_ray, 1);
    let eye_on = Renderer::new(&eye_scene, Camera::default())
        .mirror_transport(&eye_transport)
        .max_bounces(1)
        .trace_ray_seeded(mirror_ray, 1);
    g.check(eye_off.magnitude() > 0.1, "without mirrors the ray reaches the lit ground");
    g.check(
        eye_on.magnitude() < ABS_TOL && eye_on != eye_off,
        "the nearer mirror is hit first and reflects past the occluded ground",
    );

    // A sphere placed in front of the mirror is closer still, so both agree.
    let mut blocked_eye_scene = build_eye_scene();
    blocked_eye_scene.add(
        Object::new(
            sphere()
                .scale(&glm::vec3(0.1, 0.1, 0.1))
                .translate(&glm::vec3(1.5, 0.0, 1.5)),
        )
        .material(Material::diffuse(glm::vec3(0.3, 0.3, 0.3))),
    );
    let be_off = Renderer::new(&blocked_eye_scene, Camera::default())
        .max_bounces(1)
        .trace_ray_seeded(mirror_ray, 1);
    let be_on = Renderer::new(&blocked_eye_scene, Camera::default())
        .mirror_transport(&eye_transport)
        .max_bounces(1)
        .trace_ray_seeded(mirror_ray, 1);
    g.check(vec_close(be_off, be_on, 1e-12), "a nearer ordinary object still wins over the mirror");

    // ---- Mirrors reflect environment light ----
    let mut env_scene = Scene::new();
    env_scene.environment = Environment::Color(glm::vec3(0.3, 0.6, 0.9));
    let env_transport = MirrorTransport::new(
        vec![vertical_panel(0.0, 1.0, 3.0, 1.0, glm::vec3(1.0, 0.5, 0.25))],
        1,
    )
    .unwrap();
    let env_color = Renderer::new(&env_scene, Camera::default())
        .mirror_transport(&env_transport)
        .max_bounces(2)
        .trace_ray_seeded(mirror_ray, 1);
    g.check(
        vec_close(env_color, glm::vec3(0.3, 0.3, 0.225), 1e-12),
        "mirror reflects environment light with its tint",
    );

    // ---- Budget boundaries ----
    let zero_reflection = MirrorTransport::new(vec![colored], 0).unwrap();
    let delta_zero_reflection = Renderer::new(&scene, Camera::default())
        .mirror_transport(&zero_reflection)
        .max_bounces(4)
        .trace_ray_seeded(ray, 42)
        - color_off;
    g.check(
        delta_zero_reflection.magnitude() < ABS_TOL,
        "max_reflections = 0 disables folded connections",
    );
    let delta_zero_bounce = Renderer::new(&scene, Camera::default())
        .mirror_transport(&transport)
        .max_bounces(0)
        .trace_ray_seeded(ray, 42)
        - color_off;
    g.check(
        delta_zero_bounce.magnitude() < ABS_TOL,
        "no remaining eye-path bounce budget disables folded connections",
    );

    // Two parallel mirrors: depth-2 connections appear only with enough bounces.
    // The occluder sits on the straight light->receiver line but clear of every
    // folded segment (the depth-2 inter-mirror segment crosses x = 0 near z = 0.75).
    let mut par_scene = Scene::new();
    par_scene.add(ground());
    par_scene.add(Light::Point(glm::vec3(12.0, 12.0, 12.0), glm::vec3(0.0, 0.0, 1.5)));
    par_scene.add(
        Object::new(
            sphere()
                .scale(&glm::vec3(0.1, 0.1, 0.1))
                .translate(&glm::vec3(0.0, 0.0, 0.2)),
        )
        .material(Material::diffuse(glm::vec3(0.1, 0.1, 0.1))),
    );
    let par_transport = MirrorTransport::new(
        vec![
            vertical_panel(-2.0, 1.5, 3.0, 1.5, glm::vec3(1.0, 1.0, 1.0)),
            vertical_panel(2.0, 1.5, 3.0, 1.5, glm::vec3(1.0, 1.0, 1.0)),
        ],
        4,
    )
    .unwrap();
    let par_ray = Ray {
        origin: glm::vec3(0.0, -2.0, 1.0),
        dir: (glm::vec3(0.0, 0.0, 0.0) - glm::vec3(0.0, -2.0, 1.0)).normalize(),
    };
    let b0 = Renderer::new(&par_scene, Camera::default())
        .mirror_transport(&par_transport)
        .max_bounces(0)
        .trace_ray_seeded(par_ray, 3);
    let b1 = Renderer::new(&par_scene, Camera::default())
        .mirror_transport(&par_transport)
        .max_bounces(1)
        .trace_ray_seeded(par_ray, 3);
    let b2 = Renderer::new(&par_scene, Camera::default())
        .mirror_transport(&par_transport)
        .max_bounces(2)
        .trace_ray_seeded(par_ray, 3);
    g.check(b0.magnitude() < ABS_TOL, "zero bounces leaves the backlit pixel black");
    g.check(b1.magnitude() > 0.0, "one bounce admits one-reflection connections");
    g.check(b2.x > b1.x + 1e-6, "a second bounce adds deeper folded connections");

    // ---- Disabled option keeps the original path and RNG sequence ----
    // A self-contained layout where the panel geometrically participates in nothing:
    // light and receiver share x = -5, the panel stands at x = 0 with a high aperture
    // (the only analytic fold would land at z = 1, far below its z range), and the eye
    // ray never reaches x = 0. Attaching it must therefore leave every pixel unchanged.
    let mut neutral_scene = Scene::new();
    neutral_scene.add(ground());
    neutral_scene.add(Light::Point(glm::vec3(8.0, 8.0, 8.0), glm::vec3(-5.0, 0.0, 2.0)));
    let neutral_panel = MirrorTransport::new(
        vec![vertical_panel(0.0, 5.0, 1.0, 0.5, glm::vec3(0.9, 0.9, 0.9))],
        2,
    )
    .unwrap();
    let neutral_receiver = glm::vec3(-5.0, 0.0, 0.0);
    let neutral_ray = Ray {
        origin: glm::vec3(-5.0, -3.0, 1.0),
        dir: (neutral_receiver - glm::vec3(-5.0, -3.0, 1.0)).normalize(),
    };
    g.check(
        neutral_panel
            .connect(&neutral_scene, &glm::vec3(-5.0, 0.0, 2.0), &neutral_receiver, 2)
            .is_empty(),
        "the neutral layout admits no folded connection",
    );
    let disabled = Renderer::new(&neutral_scene, Camera::default()).max_bounces(0);
    let attached_unused = Renderer::new(&neutral_scene, Camera::default())
        .mirror_transport(&neutral_panel)
        .max_bounces(0);
    g.check(
        vec_close(
            disabled.trace_ray_seeded(neutral_ray, 123),
            attached_unused.trace_ray_seeded(neutral_ray, 123),
            1e-12,
        ),
        "with no mirror interaction the original traced path is unchanged at a fixed seed",
    );
    g.check(
        vec_close(
            disabled.trace_ray_seeded(neutral_ray, 456),
            attached_unused.trace_ray_seeded(neutral_ray, 456),
            1e-12,
        ),
        "RNG sequence preservation holds across seeds",
    );

    // trace_ray_seeded returns linear, un-tonemapped color (values may exceed 1).
    let mut hdr_scene = Scene::new();
    hdr_scene.add(ground());
    // A bright light directly above the receiver keeps every factor but the
    // inverse-square law near 1, producing linear radiance well above the sRGB cap.
    hdr_scene.add(Light::Point(glm::vec3(20.0, 20.0, 20.0), glm::vec3(0.0, 0.0, 1.0)));
    let hdr = Renderer::new(&hdr_scene, Camera::default())
        .max_bounces(0)
        .trace_ray_seeded(
            Ray {
                origin: glm::vec3(0.0, -2.0, 0.5),
                dir: (glm::vec3(0.0, 0.0, 0.0) - glm::vec3(0.0, -2.0, 0.5)).normalize(),
            },
            1,
        );
    g.check(hdr.x > 1.0, "seeded tracing returns un-clamped linear HDR radiance");

    // Other light types keep their existing direct-light behavior with mirrors attached.
    let mut dir_scene = Scene::new();
    dir_scene.add(ground());
    dir_scene.add(Light::Directional(glm::vec3(0.7, 0.7, 0.7), glm::vec3(0.3, 0.2, -1.0)));
    let dir_ray = Ray {
        origin: glm::vec3(0.0, -2.0, 1.0),
        dir: (glm::vec3(0.0, 0.0, 0.0) - glm::vec3(0.0, -2.0, 1.0)).normalize(),
    };
    let dir_off = Renderer::new(&dir_scene, Camera::default())
        .max_bounces(0)
        .trace_ray_seeded(dir_ray, 1);
    let dir_on = Renderer::new(&dir_scene, Camera::default())
        .mirror_transport(&eye_transport)
        .max_bounces(0)
        .trace_ray_seeded(dir_ray, 1);
    g.check(
        vec_close(dir_off, dir_on, 1e-12) && dir_off.magnitude() > 0.0,
        "directional lights keep their existing behavior",
    );

    g.finish();
}
