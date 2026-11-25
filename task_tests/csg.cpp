#include "task_support/check.h"

#include "Scene.hpp"
#include "Primitives.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using namespace Renderer;

namespace {

constexpr int W = 160;
constexpr int H = 120;

struct Fixture {
    std::vector<uint16_t> fb;
    std::vector<uint16_t> depth;
    Camera camera;
    Scene scene;

    Fixture()
        : fb(W * H, 0), depth(W * H, 0xFFFF),
          scene(fb.data(), depth.data(), W, H) {
        camera.nearPlane = 10;
        camera.farPlane = 3000;
        camera.setFOV(60.0f, W);
        camera.setPosition(0, 0, -160);
        scene.setCamera(&camera);
    }

    void render() {
        std::fill(fb.begin(), fb.end(), 0);
        std::fill(depth.begin(), depth.end(), 0xFFFF);
        scene.prepareFrame();
        scene.rasterizeBand(0, H);
    }
};

std::unique_ptr<Object> box(int w, int h, int d, Material* material,
                            Vector3 position = {0, 0, 0}) {
    Object* object = Primitives::createCube(w, h, d, material);
    object->setPosition(position);
    object->calculateBoundingBox();
    return std::unique_ptr<Object>(object);
}

CsgNode leafNode(uint32_t id) {
    CsgNode node;
    node.op = CsgOp::Leaf;
    node.leaf = id;
    return node;
}

CsgNode binaryNode(CsgOp op, uint32_t left, uint32_t right) {
    CsgNode node;
    node.op = op;
    node.left = left;
    node.right = right;
    return node;
}

bool channelClose(uint16_t a, uint16_t b, int tolerance = 1) {
    int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    return std::abs(ar - br) <= tolerance &&
           std::abs(ag - bg) <= tolerance &&
           std::abs(ab - bb) <= tolerance;
}

struct RayInterval {
    double near = 0.0;
    double far = 0.0;
    bool hit = false;
};

RayInterval boxRayInterval(const Camera& cam, int x, int y,
                           Vector3 center, Vector3 half) {
    const double f = cam.fovFactor;
    const double dx = (x - W * 0.5) / f;
    const double dy = -(y - H * 0.5) / f;
    const double ox = cam.position.x;
    const double oy = cam.position.y;
    const double oz = cam.position.z;

    double tNear = -1.0e100;
    double tFar = 1.0e100;
    const double origin[3] = {(double)ox, (double)oy, (double)oz};
    const double dir[3] = {dx, dy, 1.0};
    const double lo[3] = {(double)center.x - half.x, (double)center.y - half.y, (double)center.z - half.z};
    const double hi[3] = {(double)center.x + half.x, (double)center.y + half.y, (double)center.z + half.z};

    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(dir[axis]) < 1e-12) {
            if (origin[axis] < lo[axis] || origin[axis] > hi[axis]) return {};
            continue;
        }
        double t1 = (lo[axis] - origin[axis]) / dir[axis];
        double t2 = (hi[axis] - origin[axis]) / dir[axis];
        if (t1 > t2) std::swap(t1, t2);
        tNear = std::max(tNear, t1);
        tFar = std::min(tFar, t2);
        if (tNear > tFar) return {};
    }
    if (tFar < 0.0) return {};
    return {std::max(0.0, tNear), tFar, true};
}

Object makeLShell(Material* material) {
    Object mesh;
    const int p[6][2] = {
        {-60, -60}, {60, -60}, {60, 0},
        {0, 0}, {0, 60}, {-60, 60}
    };
    const int z0 = -60, z1 = 60;
    auto add = [&](int x, int y, int z, int nx, int ny, int nz) {
        Object::Vertex v;
        v.position = {x, y, z};
        v.normal = {nx, ny, nz};
        v.uv = {0, 0};
        mesh.addVertex(v);
        return (uint16_t)(mesh.vertices.size() - 1);
    };

    // Extruded concave L polygon. Caps use a fan; sides are outward wound.
    uint16_t top[6], bottom[6];
    for (int i = 0; i < 6; ++i) {
        bottom[i] = add(p[i][0], p[i][1], z0, 0, 0, -1024);
        top[i] = add(p[i][0], p[i][1], z1, 0, 0, 1024);
    }
    for (int i = 1; i + 1 < 6; ++i) {
        mesh.addTriangle(top[0], top[i], top[i + 1], material);
        mesh.addTriangle(bottom[0], bottom[i + 1], bottom[i], material);
    }
    for (int i = 0; i < 6; ++i) {
        int j = (i + 1) % 6;
        int ex = p[j][0] - p[i][0], ey = p[j][1] - p[i][1];
        // For a CCW polygon, outward side normal is (ey, -ex) in XY.
        int nx = ey, ny = -ex;
        uint16_t a = add(p[i][0], p[i][1], z0, nx, ny, 0);
        uint16_t b = add(p[j][0], p[j][1], z0, nx, ny, 0);
        uint16_t c = add(p[j][0], p[j][1], z1, nx, ny, 0);
        uint16_t d = add(p[i][0], p[i][1], z1, nx, ny, 0);
        mesh.addTriangle(a, b, c, material);
        mesh.addTriangle(a, c, d, material);
    }
    mesh.calculateBoundingBox();
    return mesh;
}

} // namespace

int occupancy_nested_reuse_empty() {
    Fixture f;
    Material red(0xF800), blue(0x001F);
    red.shadingMode = blue.shadingMode = ShadingMode::UNLIT;
    auto a = box(140, 140, 140, &red, {0, 0, 280});
    auto b = box(90, 90, 90, &blue, {35, 0, 280});
    f.scene.addObject(a.get());
    f.scene.addObject(b.get());

    // Difference(leaf1, leaf1) must be empty even though the leaf is reused.
    CsgDesc desc;
    desc.nodes = {leafNode(1), binaryNode(CsgOp::Difference, 0, 0)};
    desc.root = 1;
    desc.bindings = {{1, a.get(), -1}, {2, b.get(), -1}};
    TASK_CHECK(f.scene.setCsgExpression(desc) == CsgError::Ok);
    f.render();
    for (int y = 45; y < 76; y += 5)
        for (int x = 65; x < 96; x += 5)
            TASK_CHECK(f.fb[y * W + x] == 0);
    TASK_CHECK(f.scene.csgEngine()->winnerAt(80, 60) == nullptr);

    // A nested intersection/reuse expression restores the same leaf.
    CsgDesc same;
    same.nodes = {
        leafNode(1), leafNode(2),
        binaryNode(CsgOp::Union, 0, 0),
        binaryNode(CsgOp::Intersect, 2, 0),
    };
    same.root = 3;
    same.bindings = {{1, a.get(), -1}, {2, b.get(), -1}};
    TASK_CHECK(f.scene.setCsgExpression(same) == CsgError::Ok);
    f.render();
    TASK_CHECK(f.fb[60 * W + 80] == 0xF800);
    const CsgWinner* winner = f.scene.csgEngine()->winnerAt(80, 60);
    TASK_CHECK(winner && winner->leaf == 1);
    return 0;
}

int occupancy_coplanar_and_common_edge() {
    Fixture f;
    f.camera.setPosition(0, 0, -120);
    Material red(0xF800), blue(0x001F);
    red.shadingMode = blue.shadingMode = ShadingMode::UNLIT;
    auto a = box(120, 120, 100, &red, {0, 0, 210});
    auto b = box(120, 120, 100, &blue, {0, 0, 310});
    f.scene.addObject(a.get());
    f.scene.addObject(b.get());

    CsgDesc unionDesc;
    unionDesc.nodes = {leafNode(1), leafNode(2), binaryNode(CsgOp::Union, 0, 1)};
    unionDesc.root = 2;
    unionDesc.bindings = {{1, a.get(), -1}, {2, b.get(), -1}};
    TASK_CHECK(f.scene.setCsgExpression(unionDesc) == CsgError::Ok);
    f.render();
    // Coplanar touching faces form a same-depth group with unchanged root.
    const CsgWinner* touch = f.scene.csgEngine()->winnerAt(80, 60);
    TASK_CHECK(touch && touch->leaf == 1);
    TASK_CHECK(std::abs(touch->depth - 280.0) <= 2.0);
    TASK_CHECK(f.fb[60 * W + 80] == 0xF800);

    // Exact projected diagonal/common edge of a cube face. Inclusive coverage
    // reports both triangles; CSG must not count that edge twice or hole out.
    CsgDesc one;
    one.nodes = {leafNode(1)};
    one.root = 0;
    one.bindings = {{1, a.get(), -1}};
    TASK_CHECK(f.scene.setCsgExpression(one) == CsgError::Ok);
    f.render();
    const CsgWinner* edge = f.scene.csgEngine()->winnerAt(90, 70);
    TASK_CHECK(edge && edge->leaf == 1);
    TASK_CHECK(edge->triangle == 2 || edge->triangle == 3);
    TASK_CHECK(std::abs(edge->depth - 280.0) <= 2.0);
    TASK_CHECK(f.fb[70 * W + 90] == 0xF800);
    return 0;
}

int occupancy_cavity_near_far_nonconvex() {
    Fixture f;
    Material red(0xF800), blue(0x001F), cyan(0x07FF);
    red.shadingMode = blue.shadingMode = cyan.shadingMode = ShadingMode::UNLIT;
    auto a = box(140, 140, 140, &red, {0, 0, 260});
    auto b = box(90, 90, 90, &blue, {35, 0, 260});
    f.scene.addObject(a.get());
    f.scene.addObject(b.get());
    f.camera.setPosition(35, 0, 260);
    f.camera.nearPlane = 5;
    f.camera.farPlane = 60;

    CsgDesc diff;
    diff.nodes = {leafNode(1), leafNode(2), binaryNode(CsgOp::Difference, 0, 1)};
    diff.root = 2;
    diff.bindings = {{1, a.get(), -1}, {2, b.get(), -1}};
    TASK_CHECK(f.scene.setCsgExpression(diff) == CsgError::Ok);
    f.render();
    const CsgWinner* cavity = f.scene.csgEngine()->winnerAt(80, 60);
    TASK_CHECK(cavity && cavity->leaf == 2 && cavity->flipNormal);
    TASK_CHECK(std::abs(cavity->depth - 45.0) <= 2.0);
    TASK_CHECK(f.fb[60 * W + 80] == 0x001F);

    // The closer-than-near B crossing updates state but is not capped/drawn;
    // the next root boundary is A's exit at depth 70.
    f.camera.nearPlane = 60;
    f.camera.farPlane = 3000;
    f.render();
    const CsgWinner* afterNear = f.scene.csgEngine()->winnerAt(80, 60);
    TASK_CHECK(afterNear && afterNear->leaf == 1 && !afterNear->flipNormal);
    TASK_CHECK(std::abs(afterNear->depth - 70.0) <= 2.0);
    TASK_CHECK(f.fb[60 * W + 80] == 0xF800);

    // A far exit beyond farPlane still participates in the reverse sweep.
    f.camera.setPosition(0, 0, 260);
    f.camera.nearPlane = 5;
    f.camera.farPlane = 60;
    CsgDesc onlyA;
    onlyA.nodes = {leafNode(1)};
    onlyA.root = 0;
    onlyA.bindings = {{1, a.get(), -1}};
    TASK_CHECK(f.scene.setCsgExpression(onlyA) == CsgError::Ok);
    f.render();
    TASK_CHECK(f.scene.csgEngine()->winnerAt(80, 60) == nullptr);

    // Small concave closed L-shell: lower arm hit, missing upper-right notch.
    Fixture nf;
    Material shellMat(0x07FF);
    shellMat.shadingMode = ShadingMode::UNLIT;
    Object shell = makeLShell(&shellMat);
    shell.position = {0, 0, 260};
    shell.calculateBoundingBox();
    nf.scene.addObject(&shell);
    CsgDesc shellDesc;
    shellDesc.nodes = {leafNode(7)};
    shellDesc.root = 0;
    shellDesc.bindings = {{7, &shell, -1}};
    TASK_CHECK(nf.scene.setCsgExpression(shellDesc) == CsgError::Ok);
    nf.render();
    TASK_CHECK(nf.scene.csgEngine()->winnerAt(80, 80) != nullptr);
    TASK_CHECK(nf.scene.csgEngine()->winnerAt(100, 45) == nullptr);
    TASK_CHECK(nf.fb[80 * W + 80] == 0x07FF);
    return 0;
}

int boundary_analytic_depth_pick_occlusion_uv() {
    Fixture f;
    Material red(0xF800);
    red.shadingMode = ShadingMode::UNLIT;
    auto a = box(140, 140, 140, &red, {0, 0, 280});
    f.scene.addObject(a.get());
    CsgDesc desc;
    desc.nodes = {leafNode(3)};
    desc.root = 0;
    desc.bindings = {{3, a.get(), -1}};
    TASK_CHECK(f.scene.setCsgExpression(desc) == CsgError::Ok);

    PickQuery queries[5] = {
        PickQuery{80, 60}, PickQuery{72, 64}, PickQuery{88, 56},
        PickQuery{68, 52}, PickQuery{94, 69}
    };
    f.scene.setPickQueries(queries, 5);
    f.render();
    Vector3 center{0, 0, 280}, half{70, 70, 70};
    for (int i = 0; i < 5; ++i) {
        int x = queries[i].x, y = queries[i].y;
        RayInterval expected = boxRayInterval(f.camera, x, y, center, half);
        const CsgWinner* w = f.scene.csgEngine()->winnerAt(x, y);
        TASK_CHECK(expected.hit && w);
        TASK_CHECK(std::abs(w->depth - expected.near) <= 2.0);
        const PickResult& p = f.scene.getPickResults()[i];
        TASK_CHECK(p.hit && p.object == a.get() && p.triangleIndex >= 0);
        TASK_CHECK(std::abs((double)p.depth - expected.near) <= 3.0);
    }

    // An ordinary opaque cube closer to the camera wins real depth competition.
    Material green(0x07E0);
    green.shadingMode = ShadingMode::UNLIT;
    auto occluder = box(40, 40, 40, &green, {0, 0, 220});
    f.scene.addObject(occluder.get());
    PickQuery oq[2] = {PickQuery{80, 60}, PickQuery{104, 60}};
    f.scene.setPickQueries(oq, 2);
    f.render();
    TASK_CHECK(f.fb[60 * W + 80] == 0x07E0);
    TASK_CHECK(f.scene.getPickResults()[0].object == occluder.get());
    TASK_CHECK(f.fb[60 * W + 104] == 0xF800);
    TASK_CHECK(f.scene.getPickResults()[1].object == a.get());

    // Textured CSG boundary samples the ordinary texture path.
    uint16_t texels[16] = {
        0xF800, 0xF9E0, 0xFA00, 0xFBE0,
        0x07E0, 0x07FF, 0x07F0, 0x07E8,
        0x001F, 0x281F, 0x501F, 0x781F,
        0xFFFF, 0xBDF7, 0x8410, 0x0000
    };
    Texture texture(4, 4, texels);
    Material textured(0xFFFF, &texture);
    textured.shadingMode = ShadingMode::UNLIT;
    auto uvBox = box(120, 120, 120, &textured, {0, 0, 500});
    f.scene.addObject(uvBox.get());
    CsgDesc uvDesc;
    uvDesc.nodes = {leafNode(9)};
    uvDesc.root = 0;
    uvDesc.bindings = {{9, uvBox.get(), -1}};
    TASK_CHECK(f.scene.setCsgExpression(uvDesc) == CsgError::Ok);
    f.render();
    uint16_t color = f.fb[60 * W + 80];
    bool textureColor = false;
    for (uint16_t texel : texels)
        if (channelClose(color, texel, 1)) textureColor = true;
    TASK_CHECK(textureColor);
    TASK_CHECK(f.scene.csgEngine()->winnerAt(80, 60) != nullptr);
    return 0;
}

int boundary_moving_shared_instance_and_bands() {
    Fixture f;
    Material red(0xF800), blue(0x001F);
    red.shadingMode = blue.shadingMode = ShadingMode::UNLIT;
    auto authored = box(120, 120, 120, &red);
    Object::SharedMesh mesh = Object::freezeMesh(std::move(*authored));

    Object owner;
    owner.position = {0, 0, 300};
    TASK_CHECK(owner.addInstance(mesh, Object::InstanceTransform::rotated({}, {0, 0, 0}), &red) == true);
    TASK_CHECK(owner.addInstance(mesh, Object::InstanceTransform::rotated({}, {70, 0, 0}), &blue) == true);
    owner.calculateBoundingBox();
    f.scene.addObject(&owner);
    f.camera.setPosition(0, 0, -180);

    CsgDesc desc;
    desc.nodes = {leafNode(1), leafNode(2), binaryNode(CsgOp::Difference, 0, 1)};
    desc.root = 2;
    desc.bindings = {{1, &owner, 0}, {2, &owner, 1}};
    TASK_CHECK(f.scene.setCsgExpression(desc) == CsgError::Ok);

    PickQuery q{70, 60};
    f.scene.setPickQueries(&q, 1);
    f.render();
    const CsgWinner* first = f.scene.csgEngine()->winnerAt(70, 60);
    TASK_CHECK(first && first->instance == 0 && first->mesh == mesh.get());
    TASK_CHECK(std::abs(first->depth - 420.0) <= 2.0);
    TASK_CHECK(f.scene.getPickResults()[0].instanceIndex == 0);

    // Move the shared instance rigidly between frames, without rebuilding CSG.
    owner.instances[0].transform.position.z -= 40;
    owner.calculateBoundingBox();
    f.render();
    const CsgWinner* moved = f.scene.csgEngine()->winnerAt(70, 60);
    TASK_CHECK(moved && moved->instance == 0);
    TASK_CHECK(std::abs(moved->depth - 380.0) <= 2.0);
    TASK_CHECK(f.scene.getPickResults()[0].instanceIndex == 0);
    TASK_CHECK(std::abs((double)f.scene.getPickResults()[0].depth - 380.0) <= 3.0);

    // Serial bands must reproduce the full-frame result exactly.
    std::vector<uint16_t> fullColor = f.fb;
    std::vector<uint16_t> fullDepth = f.depth;
    f.scene.prepareFrame();
    f.scene.rasterizeBand(0, H / 2);
    f.scene.rasterizeBand(H / 2, H);
    TASK_CHECK(f.fb == fullColor);
    TASK_CHECK(f.depth == fullDepth);
    return 0;
}

int boundary_difference_inner_wall_lighting() {
    Fixture f;
    Material white(0xFFFF);
    white.shadingMode = ShadingMode::FLAT;
    white.diffuse = 255;
    AmbientLight ambient({12, 12, 12});
    DirectionalLight towardCamera(Vector3(270, 0, 0), Color{255, 255, 255}, 255);
    DirectionalLight awayCamera(Vector3(90, 0, 0), Color{255, 255, 255}, 255);
    f.scene.setAmbientLight(&ambient);
    f.scene.setDirectionalLight(&towardCamera);

    auto outer = box(140, 140, 140, &white, {0, 0, 260});
    auto inner = box(90, 90, 90, &white, {35, 0, 260});
    f.scene.addObject(outer.get());
    f.scene.addObject(inner.get());
    f.camera.setPosition(35, 0, 260);
    f.camera.nearPlane = 5;
    f.camera.farPlane = 2000;
    CsgDesc desc;
    desc.nodes = {leafNode(1), leafNode(2), binaryNode(CsgOp::Difference, 0, 1)};
    desc.root = 2;
    desc.bindings = {{1, outer.get(), -1}, {2, inner.get(), -1}};
    TASK_CHECK(f.scene.setCsgExpression(desc) == CsgError::Ok);
    f.render();
    uint16_t lit = f.fb[60 * W + 80];
    int lr = (lit >> 11) & 31, lg = (lit >> 5) & 63, lb = lit & 31;
    TASK_CHECK(lr >= 28 && lg >= 56 && lb >= 28);

    f.scene.setDirectionalLight(&awayCamera);
    f.render();
    uint16_t dark = f.fb[60 * W + 80];
    int dr = (dark >> 11) & 31, dg = (dark >> 5) & 63, db = dark & 31;
    TASK_CHECK(dr <= 6 && dg <= 12 && db <= 6);
    return 0;
}

int boundary_lit_textured_source_parity() {
    Material litMaterial(0xFFFF);
    litMaterial.shadingMode = ShadingMode::FLAT;
    AmbientLight ambient({24, 24, 24});
    DirectionalLight light(Vector3(250, 35, 0), Color{255, 255, 255}, 255);
    auto litBox = box(120, 120, 120, &litMaterial, {0, 0, 300});

    Fixture ordinary;
    ordinary.scene.setAmbientLight(&ambient);
    ordinary.scene.setDirectionalLight(&light);
    ordinary.scene.addObject(litBox.get());
    ordinary.render();

    Fixture csg;
    csg.scene.setAmbientLight(&ambient);
    csg.scene.setDirectionalLight(&light);
    csg.scene.addObject(litBox.get());
    CsgDesc desc;
    desc.nodes = {leafNode(1)};
    desc.root = 0;
    desc.bindings = {{1, litBox.get(), -1}};
    TASK_CHECK(csg.scene.setCsgExpression(desc) == CsgError::Ok);
    csg.render();
    for (int y = 42; y <= 78; y += 4)
        for (int x = 62; x <= 98; x += 4)
            TASK_CHECK(channelClose(csg.fb[y * W + x], ordinary.fb[y * W + x], 1));

    uint16_t texels[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
    Texture texture(2, 2, texels);
    Material textured(0xFFFF, &texture);
    textured.shadingMode = ShadingMode::UNLIT;
    auto texBox = box(120, 120, 120, &textured, {0, 0, 300});

    Fixture texOrdinary;
    texOrdinary.scene.addObject(texBox.get());
    texOrdinary.render();
    Fixture texCsg;
    texCsg.scene.addObject(texBox.get());
    CsgDesc texDesc;
    texDesc.nodes = {leafNode(2)};
    texDesc.root = 0;
    texDesc.bindings = {{2, texBox.get(), -1}};
    TASK_CHECK(texCsg.scene.setCsgExpression(texDesc) == CsgError::Ok);
    texCsg.render();
    for (int y = 45; y <= 75; y += 5)
        for (int x = 65; x <= 95; x += 5)
            TASK_CHECK(channelClose(texCsg.fb[y * W + x], texOrdinary.fb[y * W + x], 1));
    return 0;
}

int lifecycle_validation_and_restore() {
    Fixture f;
    Material red(0xF800), blue(0x001F);
    red.shadingMode = blue.shadingMode = ShadingMode::UNLIT;
    auto a = box(80, 80, 80, &red, {0, 0, 300});
    auto b = box(60, 60, 60, &blue, {30, 0, 300});
    f.scene.addObject(a.get());
    f.scene.addObject(b.get());

    CsgDesc good;
    good.nodes = {leafNode(1)};
    good.root = 0;
    good.bindings = {{1, a.get(), -1}};
    TASK_CHECK(f.scene.setCsgExpression(good) == CsgError::Ok);
    f.render();
    TASK_CHECK(f.scene.csgEnabled());
    TASK_CHECK(f.fb[60 * W + 80] == 0xF800);

    CsgDesc badChild = good;
    badChild.nodes.push_back(binaryNode(CsgOp::Union, 0, 99));
    badChild.root = 1;
    TASK_CHECK(f.scene.setCsgExpression(badChild) == CsgError::ChildOutOfRange);
    TASK_CHECK(f.scene.csgLastError() == CsgError::ChildOutOfRange);
    TASK_CHECK(f.scene.csgEnabled());
    f.render();
    TASK_CHECK(f.fb[60 * W + 80] == 0xF800);

    CsgDesc cycle;
    cycle.nodes = {
        leafNode(1),
        binaryNode(CsgOp::Union, 2, 0),
        binaryNode(CsgOp::Intersect, 1, 0),
    };
    cycle.root = 2;
    cycle.bindings = good.bindings;
    TASK_CHECK(f.scene.setCsgExpression(cycle) == CsgError::Cycle);
    TASK_CHECK(f.scene.csgEnabled());

    CsgDesc duplicate = good;
    duplicate.bindings.push_back({1, b.get(), -1});
    TASK_CHECK(f.scene.setCsgExpression(duplicate) == CsgError::DuplicateLeafBinding);
    TASK_CHECK(f.scene.csgEnabled());

    CsgDesc unbound;
    unbound.nodes = {leafNode(42)};
    unbound.root = 0;
    TASK_CHECK(f.scene.setCsgExpression(unbound) == CsgError::UnboundLeaf);
    TASK_CHECK(f.scene.csgEnabled());

    CsgDesc badInstance = good;
    badInstance.bindings[0].instance = 7;
    TASK_CHECK(f.scene.setCsgExpression(badInstance) == CsgError::InstanceOutOfRange);
    TASK_CHECK(f.scene.csgEnabled());

    TASK_CHECK(f.scene.clearCsgExpression() == CsgError::Ok);
    TASK_CHECK(!f.scene.csgEnabled());
    f.render();
    TASK_CHECK(f.fb[60 * W + 80] == 0xF800 || f.fb[60 * W + 80] == 0x001F);
    return 0;
}

int lifecycle_clear_matches_old_path_and_scenes_isolated() {
    // Reference ordinary frame.
    Fixture reference;
    Material red(0xF800), blue(0x001F);
    red.shadingMode = blue.shadingMode = ShadingMode::UNLIT;
    auto a = box(120, 120, 120, &red, {-20, 0, 300});
    auto b = box(80, 80, 80, &blue, {45, 0, 320});
    reference.scene.addObject(a.get());
    reference.scene.addObject(b.get());
    reference.render();
    std::vector<uint16_t> expectedColor = reference.fb;
    std::vector<uint16_t> expectedDepth = reference.depth;

    Fixture f;
    f.scene.addObject(a.get());
    f.scene.addObject(b.get());
    CsgDesc desc;
    desc.nodes = {leafNode(1), leafNode(2), binaryNode(CsgOp::Union, 0, 1)};
    desc.root = 2;
    desc.bindings = {{1, a.get(), -1}, {2, b.get(), -1}};
    TASK_CHECK(f.scene.setCsgExpression(desc) == CsgError::Ok);
    TASK_CHECK(f.scene.clearCsgExpression() == CsgError::Ok);
    f.render();
    TASK_CHECK(f.fb == expectedColor);
    TASK_CHECK(f.depth == expectedDepth);

    // A second Scene has its own mask/winner/pick state.
    Fixture second;
    second.scene.addObject(a.get());
    second.scene.addObject(b.get());
    TASK_CHECK(second.scene.setCsgExpression(desc) == CsgError::Ok);
    second.render();
    TASK_CHECK(second.scene.csgEngine()->winnerAt(80, 60) != nullptr);
    TASK_CHECK(f.scene.csgEngine()->winnerAt(80, 60) == nullptr);
    return 0;
}

const struct task_case task_cases[] = {
    {"occupancy", "nested_reuse_empty", occupancy_nested_reuse_empty},
    {"occupancy", "coplanar_common_edge", occupancy_coplanar_and_common_edge},
    {"occupancy", "cavity_near_far_nonconvex", occupancy_cavity_near_far_nonconvex},
    {"boundary", "depth_pick_occlusion_texture", boundary_analytic_depth_pick_occlusion_uv},
    {"boundary", "moving_shared_instance_bands", boundary_moving_shared_instance_and_bands},
    {"boundary", "difference_inner_wall_lighting", boundary_difference_inner_wall_lighting},
    {"boundary", "lit_textured_source_parity", boundary_lit_textured_source_parity},
    {"lifecycle", "validation_failed_set_restore", lifecycle_validation_and_restore},
    {"lifecycle", "clear_old_path_scene_isolation", lifecycle_clear_matches_old_path_and_scenes_isolated},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
