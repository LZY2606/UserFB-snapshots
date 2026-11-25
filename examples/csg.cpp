#include "Scene.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace Renderer;

namespace {

// 24-vertex outward box, matching Jet's own primitive winding. The local
// -Z face is the camera-facing entry face for a camera on the -Z side.
void addBox(Object& box, int32_t hx, int32_t hy, int32_t hz, Material* mat) {
    const Vector3 nxp{-1024, 0, 0}, nxx{1024, 0, 0};
    const Vector3 nyp{0, 1024, 0}, nyy{0, -1024, 0};
    const Vector3 nzp{0, 0, 1024}, nzz{0, 0, -1024};
    auto V = [&](int x, int y, int z, Vector3 n) {
        box.addVertex({{x, y, z}, {0, 0}, n});
    };
    V(-hx,-hy, hz,nzp); V( hx,-hy, hz,nzp); V( hx, hy, hz,nzp); V(-hx, hy, hz,nzp);
    V(-hx,-hy,-hz,nzz); V( hx,-hy,-hz,nzz); V( hx, hy,-hz,nzz); V(-hx, hy,-hz,nzz);
    V(-hx,-hy,-hz,nxp); V(-hx,-hy, hz,nxp); V(-hx, hy, hz,nxp); V(-hx, hy,-hz,nxp);
    V( hx,-hy, hz,nxx); V( hx,-hy,-hz,nxx); V( hx, hy,-hz,nxx); V( hx, hy, hz,nxx);
    V(-hx, hy, hz,nyp); V( hx, hy, hz,nyp); V( hx, hy,-hz,nyp); V(-hx, hy,-hz,nyp);
    V(-hx,-hy,-hz,nyy); V( hx,-hy,-hz,nyy); V( hx,-hy, hz,nyy); V(-hx,-hy, hz,nyy);
    box.addFace(0,1,2,3,mat);
    box.addFace(5,4,7,6,mat);
    box.addFace(8,9,10,11,mat);
    box.addFace(12,13,14,15,mat);
    box.addFace(16,17,18,19,mat);
    box.addFace(20,21,22,23,mat);
    box.calculateBoundingBox();
}

int redPixels(const std::vector<uint16_t>& fb, int w, int h) {
    int n=0;
    for (uint16_t c : fb) {
        int r=(c>>11)&31, g=(c>>5)&63, b=c&31;
        if (r>20 && g<12 && b<12) ++n;
    }
    (void)w;(void)h;
    return n;
}

} // namespace

int main() {
    constexpr int W=128,H=96;
    std::vector<uint16_t> fb(W*H,0), zbuf(W*H,65535);

    Camera camera;
    camera.setPosition(0,0,0);
    camera.setRotation(0,0,0);
    camera.nearPlane=10;
    camera.farPlane=700;
    camera.setFOV(60.0f,W);

    Material red(0xf800), cyan(0x07ff);
    red.shadingMode=cyan.shadingMode=ShadingMode::UNLIT;

    Object authored;
    addBox(authored,48,48,48,&red);
    Object::SharedMesh mesh=Object::freezeMesh(std::move(authored));

    Object batch;
    // Two independently placed leaves share one immutable prototype.
    batch.addInstance(mesh, Object::InstanceTransform::rotated({0,0,0},{-28,0,270}));
    batch.addInstance(mesh, Object::InstanceTransform::rotated({0,0,0},{ 34,0,292}), &cyan);
    batch.calculateBoundingBox();

    Scene scene(fb.data(),zbuf.data(),W,H);
    scene.setCamera(&camera);
    scene.addObject(&batch);

    CsgDesc desc;
    desc.nodes = {
        CsgNode::makeLeaf(1),
        CsgNode::makeLeaf(2),
        CsgNode::makeDifference(0,1)
    };
    desc.root=2;
    CsgBinding bindings[2] = {{1,&batch,0},{2,&batch,1}};
    CsgStatus st=scene.setCsgExpression(desc,bindings,2);
    if(!st) { std::printf("CSG setup failed: %s\n",st.message.c_str()); return 1; }

    PickQuery query{W/2,H/2};
    scene.setPickQueries(&query,1);
    scene.render();
    const int red1=redPixels(fb,W,H);
    const PickResult pick1=scene.getPickResults()[0];
    if(red1<100 || !pick1.hit || pick1.object!=&batch || pick1.instanceIndex!=0 ||
       (pick1.triangleIndex!=2 && pick1.triangleIndex!=3)) {
        std::printf("frame1 CSG demo assertions failed red=%d hit=%d inst=%d tri=%d\n",
                    red1,pick1.hit,pick1.instanceIndex,pick1.triangleIndex);
        return 1;
    }

    // Move the second shared instance between frames. No CSG mesh is rebuilt;
    // prepareFrame() rereads the rigid instance transform.
    batch.instances[1].transform.position = {96,0,292};
    batch.calculateBoundingBox();
    std::fill(fb.begin(),fb.end(),0);
    std::fill(zbuf.begin(),zbuf.end(),65535);
    scene.render();
    const int red2=redPixels(fb,W,H);
    const PickResult pick2=scene.getPickResults()[0];
    if(red2 <= red1 + 20 || !pick2.hit || pick2.instanceIndex!=0) {
        std::printf("frame2 moved-instance assertion failed red1=%d red2=%d inst=%d\n",
                    red1,red2,pick2.instanceIndex);
        return 1;
    }

    std::printf("CSG demo: shared difference frame red=%d, moved frame red=%d\n",red1,red2);
    std::puts("CSG_DEMO_OK");
    return 0;
}
