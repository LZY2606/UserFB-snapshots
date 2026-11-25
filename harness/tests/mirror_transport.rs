use rpt::mirrors::{MirrorPanel, MirrorTransport};
use rpt::*;

#[path = "../../task_support/check.rs"]
mod taskcheck;

type V3 = glm::DVec3;

fn v(x: f64, y: f64, z: f64) -> V3 {
    glm::vec3(x, y, z)
}

fn panel(center: V3, edge_u: V3, edge_v: V3, reflectance: V3) -> MirrorPanel {
    MirrorPanel {
        center,
        edge_u,
        edge_v,
        reflectance,
    }
}

fn close(a: f64, b: f64) -> bool {
    (a - b).abs() <= 1e-9 * b.abs().max(1.0)
}

fn vclose(a: &V3, b: &V3) -> bool {
    (a - b).magnitude() <= 1e-9
}

/// Independent check of the law of reflection: reflecting the normalized
/// incoming direction about the normal must give the outgoing direction
fn reflects(incoming: &V3, outgoing: &V3, n: &V3) -> bool {
    vclose(
        &glm::reflect_vec(&incoming.normalize(), n),
        &outgoing.normalize(),
    )
}

fn ball(center: V3, radius: f64) -> Object {
    Object::new(
        sphere()
            .scale(&glm::vec3(radius, radius, radius))
            .translate(&center),
    )
    .material(Material::diffuse(glm::vec3(0.5, 0.5, 0.5)))
}

#[test]
fn mirror_geometry() {
    let g = taskcheck::Group::new("mirror_geometry");
    let scene = Scene::new();

    // --- constructor validation ---
    let good = || panel(v(0.0, 0.0, 0.0), v(1.0, 0.0, 0.0), v(0.0, 1.0, 0.0), v(1.0, 1.0, 1.0));
    g.check(
        MirrorTransport::new(vec![good()], 4).is_ok(),
        "valid panel and max_reflections = 4 accepted",
    );
    g.check(
        MirrorTransport::new(vec![good()], 5).is_err(),
        "max_reflections above 4 rejected",
    );
    g.check(
        MirrorTransport::new((0..9).map(|_| good()).collect(), 1).is_err(),
        "more than 8 panels rejected",
    );
    g.check(
        MirrorTransport::new(
            vec![panel(v(0.0, 0.0, 0.0), v(0.0, 0.0, 0.0), v(0.0, 1.0, 0.0), v(1.0, 1.0, 1.0))],
            1,
        )
        .is_err(),
        "zero edge vector rejected",
    );
    g.check(
        MirrorTransport::new(
            vec![panel(v(0.0, 0.0, 0.0), v(1.0, 0.0, 0.0), v(1.0, 1.0, 0.0), v(1.0, 1.0, 1.0))],
            1,
        )
        .is_err(),
        "non-orthogonal edges rejected",
    );
    g.check(
        MirrorTransport::new(
            vec![panel(v(0.0, 0.0, 0.0), v(1.0, 0.0, 0.0), v(0.0, 1.0, 0.0), v(1.0, 0.5, 1.2))],
            1,
        )
        .is_err(),
        "reflectance above 1 rejected",
    );
    g.check(
        MirrorTransport::new(
            vec![panel(v(f64::NAN, 0.0, 0.0), v(1.0, 0.0, 0.0), v(0.0, 1.0, 0.0), v(1.0, 1.0, 1.0))],
            1,
        )
        .is_err(),
        "non-finite center rejected",
    );

    // --- single mirror, checked against a 2D cross-section reference ---
    let mt = MirrorTransport::new(vec![panel(
        v(0.0, 0.0, 0.0),
        v(2.0, 0.0, 0.0),
        v(0.0, 2.0, 0.0),
        v(0.5, 0.25, 1.0),
    )], 4)
    .unwrap();
    let light = v(1.0, 1.0, 2.0);
    let recv = v(-1.0, 1.0, 2.0);
    let conns = mt.find_connections(&scene, &recv, &light, 4);
    g.check(conns.len() == 1, "single mirror yields one connection");
    let c = &conns[0];
    g.check(c.panels == vec![0], "connection visits panel 0");
    // Independent reference: in the y = 1 section, reflect the light across
    // z = 0 to (1, -2) and intersect the line to (-1, 2) with z = 0
    let image = v(1.0, 1.0, -2.0);
    let xref = image + 0.5 * (recv - image); // (0, 1, 0)
    g.check(vclose(&c.points[0], &xref), "fold point matches virtual image");
    let lref = (light - xref).magnitude() + (recv - xref).magnitude();
    g.check(
        close(c.total_length, lref) && close(c.total_length, 2.0 * 5.0f64.sqrt()),
        "total length is the full polyline length",
    );
    g.check(
        vclose(&c.reflectance, &v(0.5, 0.25, 1.0)),
        "reflectance product is the panel reflectance",
    );
    g.check(
        reflects(&(xref - light), &(recv - xref), &v(0.0, 0.0, 1.0)),
        "law of reflection holds at the fold point",
    );

    // --- two non-parallel mirrors ---
    let mt2 = MirrorTransport::new(vec![
        panel(v(0.0, 0.0, 0.0), v(3.0, 0.0, 0.0), v(0.0, 3.0, 0.0), v(1.0, 1.0, 1.0)), // z = 0
        panel(v(3.0, 0.0, 0.5), v(0.0, 1.0, 0.0), v(0.0, 0.0, 1.0), v(1.0, 1.0, 1.0)), // x = 3
    ], 4)
    .unwrap();
    let light = v(1.0, 0.0, 1.0);
    let recv = v(2.0, 0.0, 1.0);
    let conns = mt2.find_connections(&scene, &recv, &light, 4);
    let c = conns
        .iter()
        .find(|c| c.panels == vec![0, 1])
        .expect("two-mirror connection exists");
    // Independent reference: unfold (1,1) -> (1,-1) -> (5,-1) in the xz section
    let i1 = v(1.0, 0.0, -1.0);
    let i2 = v(5.0, 0.0, -1.0);
    let x2 = i2 + (2.0 / 3.0) * (recv - i2); // (3, 0, 1/3)
    let x1 = i1 + (3.0 / 4.0) * (x2 - i1); // (2.5, 0, 0)
    g.check(vclose(&c.points[0], &x1), "first fold on mirror A");
    g.check(vclose(&c.points[1], &x2), "second fold on mirror B");
    let lref = (light - x1).magnitude() + (x2 - x1).magnitude() + (recv - x2).magnitude();
    g.check(close(c.total_length, lref), "two-mirror total length");
    g.check(
        reflects(&(x1 - light), &(x2 - x1), &v(0.0, 0.0, 1.0))
            && reflects(&(x2 - x1), &(recv - x2), &v(1.0, 0.0, 0.0)),
        "law of reflection holds on both mirrors",
    );

    // --- reverse reciprocity: swapping light and receiver reverses the path ---
    let rev = mt2.find_connections(&scene, &light, &recv, 4);
    let r = rev
        .iter()
        .find(|c| c.panels == vec![1, 0])
        .expect("reciprocal connection exists");
    g.check(
        close(c.total_length, r.total_length),
        "reciprocal path has the same total length",
    );
    g.check(
        vclose(&c.points[0], &r.points[1]) && vclose(&c.points[1], &r.points[0]),
        "reciprocal path reverses the fold points",
    );

    // --- A->B->A: non-adjacent repeats are kept, adjacent ones are not ---
    let mt3 = MirrorTransport::new(vec![
        panel(v(1.5, 0.0, 0.0), v(1.0, 0.0, 0.0), v(0.0, 2.0, 0.0), v(1.0, 1.0, 1.0)), // z = 0
        panel(v(1.5, 0.0, 4.0), v(1.0, 0.0, 0.0), v(0.0, 2.0, 0.0), v(1.0, 1.0, 1.0)), // z = 4
    ], 4)
    .unwrap();
    let light = v(1.0, 0.0, 1.0);
    let recv = v(2.0, 0.0, 1.0);
    let conns = mt3.find_connections(&scene, &recv, &light, 3);
    g.check(
        conns
            .iter()
            .all(|c| c.panels.windows(2).all(|w| w[0] != w[1])),
        "no consecutive duplicate panels in any sequence",
    );
    g.check(conns.len() == 6, "all six sequences up to length 3 are valid");
    let c = conns
        .iter()
        .find(|c| c.panels == vec![0, 1, 0])
        .expect("A->B->A connection is kept");
    // Independent reference: unfold (1,1) -> (1,-1) -> (1,9) -> (1,-9), then
    // intersect back through z = 0, z = 4, z = 0
    let x3 = v(1.9, 0.0, 0.0);
    let x2 = v(1.5, 0.0, 4.0);
    let x1 = v(1.1, 0.0, 0.0);
    g.check(
        vclose(&c.points[0], &x1) && vclose(&c.points[1], &x2) && vclose(&c.points[2], &x3),
        "A->B->A fold points match the virtual image reference",
    );
    let lref = (light - x1).magnitude()
        + (x2 - x1).magnitude()
        + (x3 - x2).magnitude()
        + (recv - x3).magnitude();
    g.check(close(c.total_length, lref), "A->B->A total length");
    g.check(
        reflects(&(x1 - light), &(x2 - x1), &v(0.0, 0.0, 1.0))
            && reflects(&(x2 - x1), &(x3 - x2), &v(0.0, 0.0, 1.0))
            && reflects(&(x3 - x2), &(recv - x3), &v(0.0, 0.0, 1.0)),
        "law of reflection holds at all three folds",
    );
    g.check(
        conns.iter().any(|c| c.panels == vec![1, 0, 1]),
        "B->A->B connection is also kept",
    );

    // --- reflection and budget limits ---
    let mt1 = MirrorTransport::new(vec![
        panel(v(1.5, 0.0, 0.0), v(1.0, 0.0, 0.0), v(0.0, 2.0, 0.0), v(1.0, 1.0, 1.0)),
        panel(v(1.5, 0.0, 4.0), v(1.0, 0.0, 0.0), v(0.0, 2.0, 0.0), v(1.0, 1.0, 1.0)),
    ], 1)
    .unwrap();
    let conns = mt1.find_connections(&scene, &recv, &light, 4);
    g.check(
        conns.len() == 2 && conns.iter().all(|c| c.panels.len() == 1),
        "max_reflections = 1 keeps only single-mirror connections",
    );
    g.check(
        mt3.find_connections(&scene, &recv, &light, 0).is_empty(),
        "zero remaining budget yields no connections",
    );

    // --- rigid transform of the whole setup ---
    let m = glm::rotate_y(&glm::DMat4::identity(), std::f64::consts::FRAC_PI_2);
    let t = v(10.0, -3.0, 7.0);
    let xf = |p: &V3| (m * p.to_homogeneous()).xyz() + t;
    let xv = |d: &V3| glm::mat4_to_mat3(&m) * d;
    let mt4 = MirrorTransport::new(vec![panel(
        xf(&v(0.0, 0.0, 0.0)),
        xv(&v(2.0, 0.0, 0.0)),
        xv(&v(0.0, 2.0, 0.0)),
        v(0.5, 0.25, 1.0),
    )], 4)
    .unwrap();
    let conns = mt4.find_connections(&scene, &xf(&v(-1.0, 1.0, 2.0)), &xf(&v(1.0, 1.0, 2.0)), 4);
    g.check(conns.len() == 1, "connection survives a rigid transform");
    g.check(
        vclose(&conns[0].points[0], &xf(&v(0.0, 1.0, 0.0))),
        "fold point follows the rigid transform",
    );
    g.check(
        close(conns[0].total_length, 2.0 * 5.0f64.sqrt()),
        "rigid transform preserves the total length",
    );

    g.finish();
}

#[test]
fn mirror_visibility() {
    let g = taskcheck::Group::new("mirror_visibility");
    let empty = Scene::new();
    let mirror_a = || panel(v(0.0, 0.0, 0.0), v(2.0, 0.0, 0.0), v(0.0, 2.0, 0.0), v(1.0, 1.0, 1.0));
    let light = v(1.0, 0.0, 2.0);
    let recv = v(-1.0, 0.0, 2.0);

    // Baseline: one connection, fold at the origin, segments pass z = 1
    let mt = MirrorTransport::new(vec![mirror_a()], 4).unwrap();
    g.check(
        mt.find_connections(&empty, &recv, &light, 4).len() == 1,
        "baseline connection without occluders",
    );

    // Occluder on the first segment (light -> fold); the straight line from
    // light to receiver stays 1.0 away from the sphere center (radius 0.3)
    let mut scene = Scene::new();
    scene.add(ball(v(0.5, 0.0, 1.0), 0.3));
    g.check(
        mt.find_connections(&scene, &recv, &light, 4).is_empty(),
        "occluder on the first segment blocks the connection",
    );

    // Occluder on the second segment (fold -> receiver)
    let mut scene = Scene::new();
    scene.add(ball(v(-0.5, 0.0, 1.0), 0.3));
    g.check(
        mt.find_connections(&scene, &recv, &light, 4).is_empty(),
        "occluder on the second segment blocks the connection",
    );

    // Occluder behind the mirror does not intersect any segment
    let mut scene = Scene::new();
    scene.add(ball(v(0.0, 0.0, -2.0), 0.5));
    g.check(
        mt.find_connections(&scene, &recv, &light, 4).len() == 1,
        "object behind the mirror does not block",
    );

    // Object behind the light (past the light, seen from the receiver)
    let mut scene = Scene::new();
    scene.add(ball(v(3.0, 0.0, 2.0), 0.5));
    g.check(
        mt.find_connections(&scene, &recv, &light, 4).len() == 1,
        "object behind the light does not block",
    );

    // A second mirror panel crossing a segment truncates the connection
    let panel_b = panel(v(0.5, 0.0, 1.0), v(0.0, 1.0, 0.0), v(0.3, 0.0, 0.3), v(1.0, 1.0, 1.0));
    let mt_b = MirrorTransport::new(vec![mirror_a(), panel_b], 4).unwrap();
    g.check(
        mt_b.find_connections(&empty, &recv, &light, 4).is_empty(),
        "non-designated mirror panel blocks the segment",
    );

    // Fold point outside the finite aperture: rejected, accepted when larger
    let light2 = v(1.0, 0.0, 2.0);
    let recv2 = v(-3.0, 0.0, 4.0);
    let mt_small = MirrorTransport::new(vec![panel(
        v(0.0, 0.0, 0.0),
        v(0.3, 0.0, 0.0),
        v(0.0, 0.3, 0.0),
        v(1.0, 1.0, 1.0),
    )], 4)
    .unwrap();
    g.check(
        mt_small.find_connections(&empty, &recv2, &light2, 4).is_empty(),
        "fold point outside the aperture is rejected",
    );
    g.check(
        mt.find_connections(&empty, &recv2, &light2, 4).len() == 1,
        "same path accepted with a larger aperture",
    );

    // The aperture boundary counts as inside
    let mt_edge = MirrorTransport::new(vec![panel(
        v(0.0, 0.0, 0.0),
        v(1.0, 0.0, 0.0),
        v(0.0, 1.0, 0.0),
        v(1.0, 1.0, 1.0),
    )], 4)
    .unwrap();
    let conns = mt_edge.find_connections(&empty, &v(-1.0, 0.0, 2.0), &v(3.0, 0.0, 2.0), 4);
    g.check(
        conns.len() == 1 && vclose(&conns[0].points[0], &v(1.0, 0.0, 0.0)),
        "fold point exactly on the aperture boundary is inside",
    );

    // Two coexisting valid connections through different mirrors
    let mt2 = MirrorTransport::new(vec![
        panel(v(0.0, 0.0, 0.0), v(2.0, 0.0, 0.0), v(0.0, 2.0, 0.0), v(1.0, 1.0, 1.0)), // z = 0
        panel(v(2.0, 0.0, 2.0), v(0.0, 1.0, 0.0), v(0.0, 0.0, 1.5), v(1.0, 1.0, 1.0)), // x = 2
    ], 1)
    .unwrap();
    let conns = mt2.find_connections(&empty, &v(-1.0, 0.0, 1.0), &v(1.0, 0.0, 3.0), 4);
    g.check(conns.len() == 2, "two coexisting connections are found");
    g.check(
        conns
            .iter()
            .any(|c| c.panels == vec![0] && vclose(&c.points[0], &v(-0.5, 0.0, 0.0))),
        "first coexisting connection through mirror A",
    );
    g.check(
        conns
            .iter()
            .any(|c| c.panels == vec![1] && vclose(&c.points[0], &v(2.0, 0.0, 2.5))),
        "second coexisting connection through mirror B",
    );

    // Occluder on the middle segment of a two-mirror path; the straight line
    // from light to receiver stays at z = 1, the sphere is at z = 2
    let mt3 = MirrorTransport::new(vec![
        panel(v(1.5, 0.0, 0.0), v(1.0, 0.0, 0.0), v(0.0, 2.0, 0.0), v(1.0, 1.0, 1.0)), // z = 0
        panel(v(1.5, 0.0, 4.0), v(1.0, 0.0, 0.0), v(0.0, 2.0, 0.0), v(1.0, 1.0, 1.0)), // z = 4
    ], 4)
    .unwrap();
    let light3 = v(1.0, 0.0, 1.0);
    let recv3 = v(2.0, 0.0, 1.0);
    g.check(
        mt3.find_connections(&empty, &recv3, &light3, 3).len() == 6,
        "all sequences valid without the occluder",
    );
    let mut scene = Scene::new();
    scene.add(ball(v(1.375, 0.0, 2.0), 0.3));
    let conns = mt3.find_connections(&scene, &recv3, &light3, 3);
    g.check(
        !conns.iter().any(|c| c.panels == vec![0, 1]),
        "middle-segment occluder blocks the two-mirror connection",
    );
    g.check(
        conns.len() == 1 && conns[0].panels == vec![0],
        "only the unobstructed single-mirror connection survives",
    );

    g.finish();
}

#[test]
fn mirror_render() {
    let g = taskcheck::Group::new("mirror_render");
    let camera = Camera::default();
    let zero = v(0.0, 0.0, 0.0);

    // --- Scene A: back-lit receiver lit only through a colored mirror ---
    let mat = Material::diffuse(v(0.5, 0.5, 0.5));
    let mut scene_a = Scene::new();
    scene_a.add(Object::new(sphere()).material(mat));
    let light_a = v(3.0, 0.0, 2.0);
    scene_a.add(Light::Point(v(4.0, 4.0, 4.0), light_a));
    let refl_a = v(0.8, 0.5, 0.2);
    let mt_a = MirrorTransport::new(vec![panel(
        v(6.0 / 7.0, 0.0, -3.0),
        v(1.0, 0.0, 0.0),
        v(0.0, 1.0, 0.0),
        refl_a,
    )], 4)
    .unwrap();
    // Shading point (0,0,-1) on the unit sphere; the direct light direction
    // is behind the surface, only the folded mirror path lights it
    let p = v(0.0, 0.0, -1.0);
    let n = v(0.0, 0.0, -1.0);
    let wo = v(0.0, 0.0, -1.0);
    let ray = Ray {
        origin: v(0.0, 0.0, -2.5),
        dir: v(0.0, 0.0, 1.0),
    };
    // Independent reference: virtual image (3,0,-8), fold (6/7,0,-3)
    let fold = v(6.0 / 7.0, 0.0, -3.0);
    let total = (light_a - fold).magnitude() + (fold - p).magnitude();
    let wi = (fold - p).normalize();
    let expected = mat
        .bsdf(&n, &wo, &wi)
        .component_mul(&(v(4.0, 4.0, 4.0).component_mul(&refl_a) / (total * total)))
        * wi.dot(&n);
    let renderer_a = Renderer::new(&scene_a, camera)
        .max_bounces(1)
        .mirror_transport(&mt_a);
    let color = renderer_a.trace_ray_seeded(ray, 7);
    g.check(
        vclose(&color, &expected),
        "mirror connection matches the analytic BSDF reference",
    );
    g.check(
        color.x > 0.0 && color.x != color.y && color.y != color.z,
        "colored reflectance tints the contribution",
    );
    // White mirror variant: per-channel ratio equals the reflectance
    let mt_white = MirrorTransport::new(vec![panel(
        v(6.0 / 7.0, 0.0, -3.0),
        v(1.0, 0.0, 0.0),
        v(0.0, 1.0, 0.0),
        v(1.0, 1.0, 1.0),
    )], 4)
    .unwrap();
    let color_white = Renderer::new(&scene_a, camera)
        .max_bounces(1)
        .mirror_transport(&mt_white)
        .trace_ray_seeded(ray, 7);
    g.check(
        close(color.x / color_white.x, 0.8)
            && close(color.y / color_white.y, 0.5)
            && close(color.z / color_white.z, 0.2),
        "contribution scales per channel with the reflectance",
    );
    // Total-path-length attenuation: a farther light with its own reference
    let light_a2 = v(6.0, 0.0, 2.0);
    let mut scene_a2 = Scene::new();
    scene_a2.add(Object::new(sphere()).material(mat));
    scene_a2.add(Light::Point(v(4.0, 4.0, 4.0), light_a2));
    let fold2 = v(12.0 / 7.0, 0.0, -3.0);
    let total2 = (light_a2 - fold2).magnitude() + (fold2 - p).magnitude();
    let wi2 = (fold2 - p).normalize();
    let expected2 = mat
        .bsdf(&n, &wo, &wi2)
        .component_mul(&(v(4.0, 4.0, 4.0).component_mul(&refl_a) / (total2 * total2)))
        * wi2.dot(&n);
    let color2 = Renderer::new(&scene_a2, camera)
        .max_bounces(1)
        .mirror_transport(&mt_a)
        .trace_ray_seeded(ray, 7);
    g.check(
        vclose(&color2, &expected2),
        "intensity falls off with the total path length squared",
    );
    // Back-lit without mirrors: exactly black
    let plain_a = Renderer::new(&scene_a, camera).max_bounces(1);
    g.check(
        plain_a.trace_ray_seeded(ray, 7) == zero,
        "back-lit point is black without the mirror transport",
    );
    // Budget boundaries: no bounce budget, or zero allowed reflections
    let no_budget = Renderer::new(&scene_a, camera)
        .max_bounces(0)
        .mirror_transport(&mt_a);
    g.check(
        no_budget.trace_ray_seeded(ray, 7) == zero,
        "max_bounces = 0 leaves no budget for the connection",
    );
    let mt_none = MirrorTransport::new(vec![panel(
        v(6.0 / 7.0, 0.0, -3.0),
        v(1.0, 0.0, 0.0),
        v(0.0, 1.0, 0.0),
        refl_a,
    )], 0)
    .unwrap();
    let no_reflections = Renderer::new(&scene_a, camera)
        .max_bounces(1)
        .mirror_transport(&mt_none);
    g.check(
        no_reflections.trace_ray_seeded(ray, 7) == zero,
        "max_reflections = 0 disables the connection",
    );

    // --- Scene B: the eye ray hits the mirror before the surface behind it ---
    let mut scene_b = Scene::new();
    scene_b.environment = Environment::Color(v(0.2, 0.4, 0.6));
    scene_b.add(
        Object::new(plane(v(0.0, 0.0, 1.0), -1.0)).material(Material::diffuse(v(0.8, 0.2, 0.2))),
    );
    let mt_b = MirrorTransport::new(vec![panel(
        v(0.0, 0.0, 0.0),
        v(2.0, 0.0, 0.0),
        v(0.0, 2.0, 0.0),
        v(0.5, 0.25, 1.0),
    )], 4)
    .unwrap();
    let ray_b = Ray {
        origin: v(0.0, 0.0, 2.0),
        dir: v(0.0, 0.0, -1.0),
    };
    let color_b = Renderer::new(&scene_b, camera)
        .max_bounces(1)
        .mirror_transport(&mt_b)
        .trace_ray_seeded(ray_b, 3);
    g.check(
        vclose(&color_b, &v(0.1, 0.1, 0.6)),
        "eye ray reflects off the mirror into the environment",
    );
    let color_b0 = Renderer::new(&scene_b, camera)
        .max_bounces(0)
        .mirror_transport(&mt_b)
        .trace_ray_seeded(ray_b, 3);
    g.check(
        color_b0 == zero,
        "mirror reflection consumes one bounce of budget",
    );
    let color_plain = Renderer::new(&scene_b, camera)
        .max_bounces(1)
        .trace_ray_seeded(ray_b, 3);
    g.check(
        !vclose(&color_plain, &v(0.1, 0.1, 0.6)),
        "without mirrors the ray reaches the surface behind",
    );

    // --- Scene B2: the reflected eye ray shades an object ---
    let mut scene_b2 = Scene::new();
    scene_b2.add(
        Object::new(sphere().translate(&v(0.0, 0.0, 5.0)))
            .material(Material::light(v(1.0, 1.0, 1.0), 2.0)),
    );
    let color_b2 = Renderer::new(&scene_b2, camera)
        .max_bounces(1)
        .mirror_transport(&mt_b)
        .trace_ray_seeded(ray_b, 3);
    g.check(
        vclose(&color_b2, &v(1.0, 0.5, 2.0)),
        "mirror reflection carries throughput to the object",
    );

    // --- Scene C: eye path and connection share the bounce budget ---
    let mat_c = Material::diffuse(v(0.5, 0.5, 0.5));
    let mut scene_c = Scene::new();
    scene_c.add(Object::new(sphere().translate(&v(0.0, 2.0, 0.0))).material(mat_c));
    let light_c = v(2.0, -5.0, -2.0);
    scene_c.add(Light::Point(v(4.0, 4.0, 4.0), light_c));
    // Perfectly black occluder blocks the direct path to the shading point
    scene_c.add(
        Object::new(
            sphere()
                .scale(&v(0.3, 0.3, 0.3))
                .translate(&v(1.0, -2.0, -1.0)),
        )
        .material(Material::light(v(0.0, 0.0, 0.0), 0.0)),
    );
    // Shading point (0,1,0) on the sphere, reached through mirror M1 below
    let p2 = v(0.0, 1.0, 0.0);
    let n2 = v(0.0, -1.0, 0.0);
    // Reference fold: virtual image of light_c across x = 3 is (4,-5,-2),
    // the line to p2 crosses x = 3 at (3,-3.5,-1.5)
    let fold_c = v(3.0, -3.5, -1.5);
    let mt_c = MirrorTransport::new(vec![
        panel(v(0.0, 0.0, 2.0), v(1.0, 0.0, 0.0), v(0.0, 0.0, 2.0), v(0.5, 0.25, 1.0)), // y = 0
        panel(v(3.0, -3.5, -1.5), v(0.0, 1.0, 0.0), v(0.0, 0.0, 1.0), v(1.0, 1.0, 1.0)), // x = 3
    ], 4)
    .unwrap();
    // Public connection query and renderer share the same path
    let conns = mt_c.find_connections(&scene_c, &p2, &light_c, 1);
    g.check(
        conns.len() == 1 && conns[0].panels == vec![1] && vclose(&conns[0].points[0], &fold_c),
        "public connection matches the reference path",
    );
    // Eye ray: hits mirror M1 at (0,0,1), reflects onto the sphere at p2
    let s2 = 0.5f64.sqrt();
    let ray_c = Ray {
        origin: v(0.0, 3.0, 4.0),
        dir: v(0.0, -s2, -s2),
    };
    let color_c1 = Renderer::new(&scene_c, camera)
        .max_bounces(1)
        .mirror_transport(&mt_c)
        .trace_ray_seeded(ray_c, 5);
    g.check(
        color_c1 == zero,
        "eye bounce exhausts the shared budget at max_bounces = 1",
    );
    let wo2 = -v(0.0, s2, -s2);
    let wi_c = (fold_c - p2).normalize();
    let total_c = (light_c - fold_c).magnitude() + (fold_c - p2).magnitude();
    let expected_c = v(0.5, 0.25, 1.0).component_mul(
        &(mat_c
            .bsdf(&n2, &wo2, &wi_c)
            .component_mul(&(v(4.0, 4.0, 4.0) / (total_c * total_c)))
            * wi_c.dot(&n2)),
    );
    let color_c2 = Renderer::new(&scene_c, camera)
        .max_bounces(2)
        .mirror_transport(&mt_c)
        .trace_ray_seeded(ray_c, 5);
    g.check(
        vclose(&color_c2, &expected_c),
        "one more bounce of budget lights the point through the mirror",
    );

    // --- Option off: original path and random sequence are unchanged ---
    let mut scene_d = Scene::new();
    scene_d.add(
        Object::new(plane(v(0.0, 0.0, 1.0), 0.0)).material(Material::diffuse(v(0.5, 0.6, 0.7))),
    );
    scene_d.add(Light::Point(v(4.0, 3.0, 2.0), v(0.0, 0.0, 2.0)));
    let ray_d = Ray {
        origin: v(0.3, -0.2, 2.0),
        dir: v(0.0, 0.0, -1.0),
    };
    let off = Renderer::new(&scene_d, camera).max_bounces(2);
    let base = off.trace_ray_seeded(ray_d, 42);
    g.check(
        base == off.trace_ray_seeded(ray_d, 42),
        "fixed seed gives deterministic results",
    );
    // An inert transport (no reflections allowed, panel out of the way)
    // leaves the traced path bit-for-bit identical
    let mt_far = MirrorTransport::new(vec![panel(
        v(0.0, 0.0, -100.0),
        v(1.0, 0.0, 0.0),
        v(0.0, 1.0, 0.0),
        v(1.0, 1.0, 1.0),
    )], 0)
    .unwrap();
    let inert = Renderer::new(&scene_d, camera)
        .max_bounces(2)
        .mirror_transport(&mt_far);
    g.check(
        inert.trace_ray_seeded(ray_d, 42) == base,
        "inert transport leaves the original path unchanged",
    );

    g.finish();
}
