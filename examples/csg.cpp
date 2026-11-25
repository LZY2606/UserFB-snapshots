// CSG shared-instance difference demo.
//
// Two placements of one frozen cube prototype are bound as distinct CSG
// leaves.  The first frame renders leaf0 - leaf1.  The second frame moves the
// first rigid instance and renders again without rebuilding the expression.
//
// This demo is headless: it renders to caller-owned RGB565/depth buffers,
// checks pixels and picking, and prints CSG_DEMO_OK.

#include "Scene.hpp"
#include "Primitives.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace Renderer;

namespace {
constexpr int W = 160;
constexpr int H = 120;

bool closeDepth(int actual, double expected) {
    return std::abs((double)actual - expected) <= 3.0;
}
}

int main() {
    std::vector<uint16_t> framebuffer(W * H, 0);
    std::vector<uint16_t> zbuffer(W * H, 0xFFFF);

    Camera camera;
    camera.nearPlane = 10;
    camera.farPlane = 3000;
    camera.setFOV(60.0f, W);
    camera.setPosition(0, 0, -180);

    Material red(0xF800);
    Material blue(0x001F);
    red.shadingMode = blue.shadingMode = ShadingMode::UNLIT;

    Object* authored = Primitives::createCube(120, 120, 120, &red);
    Object::SharedMesh sharedCube = Object::freezeMesh(std::move(*authored));
    delete authored;

    Object owner;
    owner.position = {0, 0, 300};
    if (!owner.addInstance(sharedCube,
                           Object::InstanceTransform::rotated({}, {0, 0, 0}),
                           &red)) {
        std::printf("CSG demo: failed to add instance 0\n");
        return 1;
    }
    if (!owner.addInstance(sharedCube,
                           Object::InstanceTransform::rotated({}, {70, 0, 0}),
                           &blue)) {
        std::printf("CSG demo: failed to add instance 1\n");
        return 1;
    }
    owner.calculateBoundingBox();

    Scene scene(framebuffer.data(), zbuffer.data(), W, H);
    scene.setCamera(&camera);
    scene.addObject(&owner);

    CsgNode leafA;
    leafA.op = CsgOp::Leaf;
    leafA.leaf = 1;
    CsgNode leafB = leafA;
    leafB.leaf = 2;
    CsgNode difference;
    difference.op = CsgOp::Difference;
    difference.left = 0;
    difference.right = 1;

    CsgDesc description;
    description.nodes = {leafA, leafB, difference};
    description.root = 2;
    description.bindings = {{1, &owner, 0}, {2, &owner, 1}};

    CsgError error = scene.setCsgExpression(description);
    if (error != CsgError::Ok) {
        std::printf("CSG demo: setup failed: %s\n", csgErrorName(error));
        return 1;
    }

    PickQuery query{70, 60};
    scene.setPickQueries(&query, 1);

    scene.prepareFrame();
    scene.rasterizeBand(0, H);

    const uint16_t firstPixel = framebuffer[60 * W + 70];
    const PickResult firstPick = scene.getPickResults()[0];
    const CsgWinner* firstWinner = scene.csgEngine()->winnerAt(70, 60);
    std::printf("frame 1: pixel=%04x depth=%u pick=%d instance=%d tri=%d\n",
                firstPixel, zbuffer[60 * W + 70], firstPick.hit,
                firstPick.instanceIndex, firstPick.triangleIndex);

    if (firstPixel != 0xF800 || !firstWinner ||
        firstWinner->instance != 0 ||
        !closeDepth(firstWinner->depth, 420.0) ||
        !firstPick.hit || firstPick.object != &owner ||
        firstPick.mesh != sharedCube.get() ||
        firstPick.instanceIndex != 0 || firstPick.triangleIndex < 0) {
        std::printf("CSG demo: first-frame pixel/pick assertion failed\n");
        return 1;
    }

    // Move the shared placement.  The immutable prototype and expression DAG
    // stay installed; prepareFrame() recomputes only rigid placement.
    owner.instances[0].transform.position.z -= 40;
    owner.calculateBoundingBox();

    scene.prepareFrame();
    scene.rasterizeBand(0, H);

    const uint16_t secondPixel = framebuffer[60 * W + 70];
    const PickResult secondPick = scene.getPickResults()[0];
    const CsgWinner* secondWinner = scene.csgEngine()->winnerAt(70, 60);
    std::printf("frame 2: pixel=%04x depth=%u pick=%d instance=%d tri=%d\n",
                secondPixel, zbuffer[60 * W + 70], secondPick.hit,
                secondPick.instanceIndex, secondPick.triangleIndex);

    if (secondPixel != 0xF800 || !secondWinner ||
        secondWinner->instance != 0 ||
        !closeDepth(secondWinner->depth, 380.0) ||
        !secondPick.hit || secondPick.object != &owner ||
        secondPick.mesh != sharedCube.get() ||
        secondPick.instanceIndex != 0 ||
        !closeDepth(secondPick.depth, 380.0)) {
        std::printf("CSG demo: second-frame pixel/pick assertion failed\n");
        return 1;
    }

    std::printf("CSG_DEMO_OK\n");
    return 0;
}
