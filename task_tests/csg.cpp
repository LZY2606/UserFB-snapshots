#include "task_support/check.h"

#include "Scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace Renderer;

namespace {

constexpr int W = 96;
constexpr int H = 72;
constexpr int CX = W / 2;
constexpr int CY = H / 2;

struct Fixture {
    std::vector<uint16_t> color;
    std::vector<uint16_t> depth;
    Camera camera;
    Scene scene;

    Fixture()
        : color(W * H, 0), depth(W * H, 65535),
          scene(color.data(), depth.data(), W, H) {
        camera.setPosition(0, 0, 0);
        camera.setRotation(0, 0, 0);
        camera.nearPlane = 10;
        camera.farPlane = 600;
        camera.setFOV(60.0f, W);
        scene.setCamera(&camera);
    }
    void clearBuffers() {
        std::fill(color.begin(), color.end(), 0);
        std::fill(depth.begin(), depth.end(), 65535);
    }
    void frame() {
        clearBuffers();
        scene.prepareFrame();
        scene.rasterizeBand(0, H);
    }
    uint16_t px(int x, int y) const { return color[static_cast<size_t>(y) * W + x]; }
    uint16_t zAt(int x, int y) const { return depth[static_cast<size_t>(y) * W + x]; }
};

bool channelClose565(uint16_t got, uint16_t expected, int channelTolerance = 1) {
    int gr = (got >> 11) & 31, gg = (got >> 5) & 63, gb = got & 31;
    int er = (expected >> 11) & 31, eg = (expected >> 5) & 63, eb = expected & 31;
    return std::abs(gr - er) <= channelTolerance &&
           std::abs(gg - eg) <= channelTolerance * 2 + 1 &&
           std::abs(gb - eb) <= channelTolerance;
}

bool isRed(uint16_t c) { return channelClose565(c, 0xf800, 2); }
bool isGreen(uint16_t c) { return channelClose565(c, 0x07e0, 2); }
bool isBlue(uint16_t c) { return channelClose565(c, 0x001f, 2); }
bool isCyan(uint16_t c) { return channelClose565(c, 0x07ff, 2); }
bool isYellow(uint16_t c) { return channelClose565(c, 0xffe0, 2); }
bool isGray(uint16_t c) {
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return std::abs(r - 16) <= 3 && std::abs(g - 32) <= 5 && std::abs(b - 16) <= 3;
}

struct BoxR {
    double x0, x1, y0, y1, z0, z1;
};

// Independent analytic ray interval. At identity camera, pixel (px,py) is
// x = ((px-CX)/f)*z and y = ((CY-py)/f)*z. This deliberately does not call
// any Jet triangle intersection code.
bool rayBox(const BoxR& b, int px, int py, float fovFactor,
            double& nearZ, double& farZ) {
    const double ax = (double(px) - CX) / double(fovFactor);
    const double ay = (CY - double(py)) / double(fovFactor);
    double lo = -1e100, hi = 1e100;
    auto slab = [&](double coefficient, double mn, double mx) {
        if (std::abs(coefficient) < 1e-12) {
            return mn <= 0.0 && 0.0 <= mx; // ray coordinate is coefficient*z, origin zero
        }
        double a = mn / coefficient;
        double c = mx / coefficient;
        if (a > c) std::swap(a, c);
        lo = std::max(lo, a);
        hi = std::min(hi, c);
        return lo <= hi + 1e-9;
    };
    if (!slab(ax, b.x0, b.x1)) return false;
    if (!slab(ay, b.y0, b.y1)) return false;
    lo = std::max(lo, b.z0);
    hi = std::min(hi, b.z1);
    nearZ = lo;
    farZ = hi;
    return hi >= std::max(0.0, lo) - 1e-9;
}

void addBox(Object& box, int hx, int hy, int hz, Material* mat) {
    const Vector3 nxp{-1024,0,0}, nxx{1024,0,0};
    const Vector3 nyp{0,1024,0}, nyy{0,-1024,0};
    const Vector3 nzp{0,0,1024}, nzz{0,0,-1024};
    auto uvTop = Vector2{1024, 1024};
    auto uvZero = Vector2{0, 0};
    auto uvU = Vector2{1024, 0};
    auto uvV = Vector2{0, 1024};
    auto V = [&](int x, int y, int z, Vector3 n, Vector2 uv) {
        Object::Vertex v;
        v.position = {x, y, z};
        v.normal = n;
        v.uv = uv;
        box.addVertex(v);
    };
    // Match Jet's outward cube table and UV orientation.
    V(-hx,-hy, hz,nzp,uvV); V( hx,-hy, hz,nzp,uvTop);
    V( hx, hy, hz,nzp,uvU); V(-hx, hy, hz,nzp,uvZero);
    V(-hx,-hy,-hz,nzz,uvV); V( hx,-hy,-hz,nzz,uvTop);
    V( hx, hy,-hz,nzz,uvU); V(-hx, hy,-hz,nzz,uvZero);
    V(-hx,-hy,-hz,nxp,uvV); V(-hx,-hy, hz,nxp,uvTop);
    V(-hx, hy, hz,nxp,uvU); V(-hx, hy,-hz,nxp,uvZero);
    V( hx,-hy, hz,nxx,uvV); V( hx,-hy,-hz,nxx,uvTop);
    V( hx, hy,-hz,nxx,uvU); V( hx, hy, hz,nxx,uvZero);
    V(-hx, hy, hz,nyp,uvV); V( hx, hy, hz,nyp,uvTop);
    V( hx, hy,-hz,nyp,uvU); V(-hx, hy,-hz,nyp,uvZero);
    V(-hx,-hy,-hz,nyy,uvV); V( hx,-hy,-hz,nyy,uvTop);
    V( hx,-hy, hz,nyy,uvU); V(-hx,-hy, hz,nyy,uvZero);
    box.addFace(0,1,2,3,mat);
    box.addFace(5,4,7,6,mat);
    box.addFace(8,9,10,11,mat);
    box.addFace(12,13,14,15,mat);
    box.addFace(16,17,18,19,mat);
    box.addFace(20,21,22,23,mat);
    box.calculateBoundingBox();
}

void addCenteredBox(Object& o, int cx, int cy, int cz,
                    int hx, int hy, int hz, Material* m) {
    Object local;
    addBox(local, hx, hy, hz, m);
    o.vertices.reserve(o.vertices.size() + local.vertices.size());
    const uint16_t base = static_cast<uint16_t>(o.vertices.size());
    for (auto v : local.vertices) {
        v.position.x += cx;
        v.position.y += cy;
        v.position.z += cz;
        o.addVertex(v);
    }
    for (auto t : local.triangles) {
        o.addTriangle(base + t.v1, base + t.v2, base + t.v3, m);
    }
    o.calculateBoundingBox();
}

BoxR centeredBox(int cx, int cy, int cz, int hx, int hy, int hz) {
    return {double(cx-hx), double(cx+hx), double(cy-hy), double(cy+hy),
            double(cz-hz), double(cz+hz)};
}

// Small non-convex closed L prism, extruded along Z.
void addLPrism(Object& o, int z0, int z1, Material* m) {
    const int P[6][2] = {{-50,-50},{50,-50},{50,0},{0,0},{0,50},{-50,50}};
    const uint16_t base = static_cast<uint16_t>(o.vertices.size());
    for (int i = 0; i < 6; ++i) {
        Object::Vertex a, b;
        a.position = {P[i][0], P[i][1], z0};
        b.position = {P[i][0], P[i][1], z1};
        o.addVertex(a); o.addVertex(b);
    }
    auto idx = [&](int edge, int far) { return uint16_t(base + edge * 2 + far); };
    for (int i = 0; i < 6; ++i) {
        int j = (i + 1) % 6;
        // Outward side winding for CCW XY polygon.
        o.addTriangle(idx(i,0), idx(j,0), idx(j,1), m);
        o.addTriangle(idx(i,0), idx(j,1), idx(i,1), m);
    }
    // Two rectangular cap regions avoid triangulating the concave cap.
    auto capQuad = [&](int x0, int y0, int x1, int y1, int z, bool farCap) {
        uint16_t b = static_cast<uint16_t>(o.vertices.size());
        int xs[4] = {x0,x1,x1,x0}, ys[4] = {y0,y0,y1,y1};
        for (int i=0;i<4;++i) {
            Object::Vertex v; v.position={xs[i],ys[i],z}; o.addVertex(v);
        }
        if (farCap) {
            o.addTriangle(b+0,b+1,b+2,m); o.addTriangle(b+0,b+2,b+3,m);
        } else {
            o.addTriangle(b+0,b+3,b+2,m); o.addTriangle(b+0,b+2,b+1,m);
        }
    };
    capQuad(-50,-50,  0, 50,z0,false);
    capQuad(  0,-50, 50,  0,z0,false);
    capQuad(-50,-50,  0, 50,z1,true);
    capQuad(  0,-50, 50,  0,z1,true);
    o.calculateBoundingBox();
}

CsgNode leaf(uint32_t id) { return CsgNode::makeLeaf(id); }
CsgNode op(CsgOp o, uint32_t a, uint32_t b) { return CsgNode::make(o,a,b); }

bool installThree(Fixture& f, Object& a, Object& b, Object& c,
                  CsgOp top = CsgOp::Intersection) {
    CsgDesc d;
    d.nodes = {leaf(1), leaf(2), CsgNode::makeUnion(0,1), leaf(3),
               CsgNode::make(top,2,3)};
    d.root = 4;
    CsgBinding bs[3] = {{1,&a,-1},{2,&b,-1},{3,&c,-1}};
    return f.scene.setCsgExpression(d, bs, 3).ok();
}

int occupancy_nested_and_self() {
    Fixture f;
    Material red(0xf800), green(0x07e0), blue(0x001f);
    red.shadingMode = green.shadingMode = blue.shadingMode = ShadingMode::UNLIT;
    Object a,b,c;
    addCenteredBox(a,-35,0,240,55,48,60,&red);
    addCenteredBox(b, 35,0,260,55,48,60,&green);
    addCenteredBox(c,  0,0,250,42,42,40,&blue);
    f.scene.addObject(&a); f.scene.addObject(&b); f.scene.addObject(&c);
    TASK_CHECK(installThree(f,a,b,c));
    f.frame();
    TASK_CHECK(isBlue(f.px(CX,CY)));
    const PickResult& p = f.scene.getPickResults()[0];
    PickQuery q{CX,CY}; f.scene.setPickQueries(&q,1);
    f.frame();
    TASK_CHECK(isBlue(f.px(CX,CY)));
    TASK_CHECK(p.hit && p.object == &c);

    // Reusing one leaf twice in A ∪ A must be the same occupancy/source.
    CsgDesc reuse;
    reuse.nodes = {leaf(7), CsgNode::makeUnion(0,0)};
    reuse.root = 1;
    CsgBinding rb{7,&a,-1};
    TASK_CHECK(f.scene.setCsgExpression(reuse,&rb,1).ok());
    f.frame();
    TASK_CHECK(isRed(f.px(CX,CY)));

    // A - A is empty even though the leaf is referenced in both child slots.
    // Use an isolated Scene so b/c from the nested expression are not present
    // as ordinary unbound objects.
    {
        Fixture self;
        self.scene.addObject(&a);
        CsgDesc empty;
        empty.nodes = {leaf(7), CsgNode::makeDifference(0,0)};
        empty.root = 1;
        CsgBinding selfBinding{7,&a,-1};
        TASK_CHECK(self.scene.setCsgExpression(empty,&selfBinding,1).ok());
        PickQuery q{CX,CY}; self.scene.setPickQueries(&q,1);
        self.frame();
        TASK_CHECK(self.px(CX,CY) == 0);
        TASK_CHECK(self.zAt(CX,CY) == 65535);
        TASK_CHECK(!self.scene.getPickResults()[0].hit);
    }
    return 0;
}

int occupancy_coplanar_and_nonconvex() {
    Fixture f;
    Material red(0xf800), green(0x07e0), yellow(0xffe0);
    red.shadingMode = green.shadingMode = yellow.shadingMode = ShadingMode::UNLIT;
    Object left, right, lshell;
    addCenteredBox(left,-40,0,270,40,45,50,&red);   // x [-80,0]
    addCenteredBox(right,40,0,270,40,45,50,&green);  // x [0,80], coplanar contact
    addLPrism(lshell,220,320,&yellow);
    f.scene.addObject(&left); f.scene.addObject(&right); f.scene.addObject(&lshell);

    CsgDesc d;
    d.nodes = {leaf(1),leaf(2),CsgNode::makeUnion(0,1)};
    d.root=2;
    CsgBinding bs[2]={{1,&left,-1},{2,&right,-1}};
    TASK_CHECK(f.scene.setCsgExpression(d,bs,2).ok());
    f.frame();
    // Pixels just on either side keep their own material.
    TASK_CHECK(isRed(f.px(CX-4,CY)));
    TASK_CHECK(isGreen(f.px(CX+4,CY)));
    // The shared seam must not become a background/fake surface.
    TASK_CHECK(f.px(CX,CY) != 0);
    TASK_CHECK(f.zAt(CX,CY) < 65535);

    Fixture lf;
    lf.scene.addObject(&lshell);
    CsgDesc ld;
    ld.nodes={leaf(9)}; ld.root=0;
    CsgBinding lb{9,&lshell,-1};
    TASK_CHECK(lf.scene.setCsgExpression(ld,&lb,1).ok());
    lf.frame();
    // World (-25,-25,z~270) is L body; (+25,+25) is missing upper-right.
    const int bodyX = CX + int(-25 * lf.camera.fovFactor / 270);
    const int bodyY = CY - int(-25 * lf.camera.fovFactor / 270);
    const int holeX = CX + int( 25 * lf.camera.fovFactor / 270);
    const int holeY = CY - int( 25 * lf.camera.fovFactor / 270);
    TASK_CHECK(isYellow(lf.px(bodyX,bodyY)));
    TASK_CHECK(lf.px(holeX,holeY) == 0);
    return 0;
}

int occupancy_cavity_near_far() {
    Fixture f;
    f.camera.nearPlane=10; f.camera.farPlane=420;
    Material outer(0xf800), cutter(0x07e0);
    outer.shadingMode=cutter.shadingMode=ShadingMode::UNLIT;
    Object a,b;
    addCenteredBox(a,0,0,120,150,150,240,&outer);  // z [-120,360]
    addCenteredBox(b,0,0,80,95,95,140,&cutter);     // z [-60,220]
    f.scene.addObject(&a); f.scene.addObject(&b);
    CsgDesc d; d.nodes={leaf(1),leaf(2),CsgNode::makeDifference(0,1)}; d.root=2;
    CsgBinding bs[2]={{1,&a,-1},{2,&b,-1}};
    TASK_CHECK(f.scene.setCsgExpression(d,bs,2).ok());
    PickQuery q{CX,CY}; f.scene.setPickQueries(&q,1);
    f.frame();
    // Camera is inside both operands; the visible difference boundary is the
    // cutter's far/exit wall at analytic z=220.
    TASK_CHECK(isGreen(f.px(CX,CY)));
    const PickResult& p=f.scene.getPickResults()[0];
    TASK_CHECK(p.hit && p.object==&b);
    TASK_CHECK(std::abs(p.depth-220)<=2);

    // A camera inside a leaf whose only exit is beyond farPlane must not get
    // a fabricated far-plane cap or depth.
    Fixture far;
    far.camera.nearPlane=10; far.camera.farPlane=300;
    Material white(0xffff); white.shadingMode=ShadingMode::UNLIT;
    Object big; addCenteredBox(big,0,0,410,170,170,490,&white); // z [-80,900]
    far.scene.addObject(&big);
    CsgDesc one; one.nodes={leaf(1)}; one.root=0;
    CsgBinding ob{1,&big,-1};
    TASK_CHECK(far.scene.setCsgExpression(one,&ob,1).ok());
    far.frame();
    TASK_CHECK(far.px(CX,CY)==0);
    TASK_CHECK(far.zAt(CX,CY)==65535);

    // Entry before nearPlane still establishes occupancy; the far exit is the
    // in-range visible boundary and near plane itself is not capped.
    Fixture near;
    near.camera.nearPlane=40; near.camera.farPlane=600;
    Object small; addCenteredBox(small,0,0,120,45,45,100,&white); // z [20,220]
    near.scene.addObject(&small);
    CsgBinding nb{1,&small,-1};
    TASK_CHECK(near.scene.setCsgExpression(one,&nb,1).ok());
    PickQuery nq{CX,CY}; near.scene.setPickQueries(&nq,1);
    near.frame();
    TASK_CHECK(channelClose565(near.px(CX,CY),0xffff,1));
    TASK_CHECK(std::abs(near.scene.getPickResults()[0].depth-220)<=2);
    return 0;
}

int boundary_depth_tie_order() {
    Fixture f;
    Material red(0xf800), green(0x07e0);
    red.shadingMode=green.shadingMode=ShadingMode::UNLIT;
    Object highIdObject, lowIdObject;
    addCenteredBox(highIdObject,0,0,260,50,50,50,&red);
    addCenteredBox(lowIdObject,0,0,260,50,50,50,&green);
    // Deliberately submit the high-numbered leaf first.
    f.scene.addObject(&highIdObject);
    f.scene.addObject(&lowIdObject);
    CsgDesc d;
    d.nodes={leaf(30),leaf(2),CsgNode::makeUnion(0,1)}; d.root=2;
    CsgBinding bs[2]={{30,&highIdObject,-1},{2,&lowIdObject,-1}};
    TASK_CHECK(f.scene.setCsgExpression(d,bs,2).ok());
    const int tx=CX-11, ty=CY+7; // face interior, away from the quad diagonal
    PickQuery q{int16_t(tx),int16_t(ty)}; f.scene.setPickQueries(&q,1);
    f.frame();
    double zn,zf;
    TASK_CHECK(rayBox(centeredBox(0,0,260,50,50,50),tx,ty,f.camera.fovFactor,zn,zf));
    // Atomic group semantics plus the mandated ascending simulation make the
    // final root-changing source the higher stable leaf number.
    TASK_CHECK(isRed(f.px(tx,ty)));
    const PickResult& p=f.scene.getPickResults()[0];
    TASK_CHECK(p.hit && p.object==&highIdObject);
    TASK_CHECK(std::abs(p.depth-int(zn))<=2);

    // Reverse Scene/object submission order; stable leaf number still wins.
    Fixture g;
    g.scene.addObject(&lowIdObject);
    g.scene.addObject(&highIdObject);
    TASK_CHECK(g.scene.setCsgExpression(d,bs,2).ok());
    g.scene.setPickQueries(&q,1);
    g.frame();
    TASK_CHECK(isRed(g.px(tx,ty)));
    TASK_CHECK(g.scene.getPickResults()[0].object==&highIdObject);
    return 0;
}

int boundary_light_uv_occlusion() {
    // Flipped difference inner wall must face the cavity light.
    {
        Fixture f;
        Material white(0xffff);
        white.shadingMode=ShadingMode::FLAT;
        white.diffuse=255;
        Object a,b;
        addCenteredBox(a,0,0,120,150,150,240,&white);
        addCenteredBox(b,0,0,80,95,95,140,&white);
        f.scene.addObject(&a); f.scene.addObject(&b);
        CsgDesc d; d.nodes={leaf(1),leaf(2),CsgNode::makeDifference(0,1)}; d.root=2;
        CsgBinding bs[2]={{1,&a,-1},{2,&b,-1}};
        TASK_CHECK(f.scene.setCsgExpression(d,bs,2).ok());
        DirectionalLight towardCamera({270,0,0},{255,255,255},255);
        AmbientLight ambient({28,28,28});
        f.scene.setDirectionalLight(&towardCamera);
        f.scene.setAmbientLight(&ambient);
        PickQuery q{CX,CY}; f.scene.setPickQueries(&q,1);
        f.frame();
        uint16_t lit=f.px(CX,CY);
        TASK_CHECK(((lit>>5)&63) > 45);
        TASK_CHECK(f.scene.getPickResults()[0].object==&b);

        // Opposite directional light leaves the flipped normal dark.
        towardCamera.updateDirection({90,0,0});
        f.frame();
        uint16_t dark=f.px(CX,CY);
        TASK_CHECK(((dark>>5)&63) < 24);
    }

    // PHONG is inside the promised first scope and uses the same masked
    // boundary/interpolation path.
    {
        Fixture f;
        Material phong(0xffff);
        phong.shadingMode=ShadingMode::PHONG;
        phong.specular=96;
        phong.specularExponent=48;
        Object a,b;
        addCenteredBox(a,0,0,120,150,150,240,&phong);
        addCenteredBox(b,0,0,80,95,95,140,&phong);
        f.scene.addObject(&a); f.scene.addObject(&b);
        CsgDesc d; d.nodes={leaf(1),leaf(2),CsgNode::makeDifference(0,1)}; d.root=2;
        CsgBinding bs[2]={{1,&a,-1},{2,&b,-1}};
        TASK_CHECK(f.scene.setCsgExpression(d,bs,2).ok());
        DirectionalLight light({270,20,0},{255,255,255},255);
        AmbientLight ambient({35,35,35});
        f.scene.setDirectionalLight(&light);
        f.scene.setAmbientLight(&ambient);
        PickQuery q{CX,CY}; f.scene.setPickQueries(&q,1);
        f.frame();
        TASK_CHECK(f.px(CX,CY) != 0);
        TASK_CHECK(((f.px(CX,CY)>>5)&63) > 28);
        TASK_CHECK(f.scene.getPickResults()[0].object==&b);
    }

    // Perspective UVs and source triangle attribution survive CSG routing.
    {
        Fixture f;
        uint16_t texels[4]={0xf800,0x07e0,0x001f,0xffff};
        Texture texture(2,2,texels);
        Material textured(0xffff,&texture);
        textured.shadingMode=ShadingMode::UNLIT;
        Object box; addCenteredBox(box,0,0,260,50,50,50,&textured);
        f.scene.addObject(&box);
        CsgDesc d; d.nodes={leaf(4)}; d.root=0;
        CsgBinding b{4,&box,-1};
        TASK_CHECK(f.scene.setCsgExpression(d,&b,1).ok());
        PickQuery q{int16_t(CX + 10), int16_t(CY - 8)};
        f.scene.setPickQueries(&q,1);
        f.frame();
        // This interior UV is row 0 / column 1 of the 2x2 fixture texture.
        TASK_CHECK(channelClose565(f.px(CX + 10, CY - 8), 0x07e0, 1));
        const PickResult& p=f.scene.getPickResults()[0];
        TASK_CHECK(p.hit && p.object==&box &&
                   (p.triangleIndex==2 || p.triangleIndex==3));
    }

    // An ordinary opaque object at real nearer depth wins colour and pick.
    {
        Fixture f;
        Material csgRed(0xf800), ordinaryGray(0x8410);
        csgRed.shadingMode=ordinaryGray.shadingMode=ShadingMode::UNLIT;
        Object csgBox, occluder;
        addCenteredBox(csgBox,0,0,280,55,55,55,&csgRed);
        addCenteredBox(occluder,0,0,185,34,34,30,&ordinaryGray); // z 155..215
        f.scene.addObject(&csgBox); f.scene.addObject(&occluder);
        CsgDesc d; d.nodes={leaf(1)}; d.root=0;
        CsgBinding b{1,&csgBox,-1};
        TASK_CHECK(f.scene.setCsgExpression(d,&b,1).ok());
        PickQuery q{CX,CY}; f.scene.setPickQueries(&q,1);
        f.frame();
        TASK_CHECK(isGray(f.px(CX,CY)));
        const PickResult& p=f.scene.getPickResults()[0];
        TASK_CHECK(p.hit && p.object==&occluder && p.instanceIndex==-1);
        TASK_CHECK(f.zAt(CX,CY) < 220);
    }
    return 0;
}

int boundary_shared_instances_bands() {
    Fixture f;
    Material red(0xf800), cyan(0x07ff);
    red.shadingMode=cyan.shadingMode=ShadingMode::UNLIT;
    Object authored; addBox(authored,45,45,45,&red);
    Object::SharedMesh mesh=Object::freezeMesh(std::move(authored));
    Object batch;
    TASK_CHECK(batch.addInstance(mesh,
        Object::InstanceTransform::rotated({0,0,0},{-25,0,260})));
    TASK_CHECK(batch.addInstance(mesh,
        Object::InstanceTransform::rotated({0,0,0},{ 35,0,285}),&cyan));
    batch.calculateBoundingBox();
    f.scene.addObject(&batch);
    CsgDesc d; d.nodes={leaf(11),leaf(12),CsgNode::makeDifference(0,1)}; d.root=2;
    CsgBinding bs[2]={{11,&batch,0},{12,&batch,1}};
    TASK_CHECK(f.scene.setCsgExpression(d,bs,2).ok());
    PickQuery q{CX,CY}; f.scene.setPickQueries(&q,1);

    f.scene.prepareFrame();
    f.scene.rasterizeBand(0,H);
    const std::vector<uint16_t> fullColor=f.color;
    const std::vector<uint16_t> fullDepth=f.depth;
    const PickResult fullPick=f.scene.getPickResults()[0];
    TASK_CHECK(fullPick.hit && fullPick.instanceIndex==0);
    TASK_CHECK(isRed(f.px(CX,CY)));

    // Serial disjoint bands must reproduce the full-frame result.
    f.scene.prepareFrame();
    f.scene.rasterizeBand(0,H/2);
    f.scene.rasterizeBand(H/2,H);
    TASK_CHECK(f.color==fullColor);
    TASK_CHECK(f.depth==fullDepth);

    // Move one existing shared instance between frames and prepare again.
    batch.instances[1].transform.position = {95,0,285};
    batch.calculateBoundingBox();
    f.frame();
    TASK_CHECK(isRed(f.px(CX,CY)));
    double zn,zf;
    BoxR first=centeredBox(-25,0,260,45,45,45);
    TASK_CHECK(rayBox(first,CX,CY,f.camera.fovFactor,zn,zf));
    const PickResult& moved=f.scene.getPickResults()[0];
    TASK_CHECK(moved.hit && moved.instanceIndex==0);
    TASK_CHECK(std::abs(moved.depth-int(zn))<=2);
    TASK_CHECK(f.zAt(CX,CY) <= uint16_t(std::max(0,std::min(65535,int(zn))))+1);
    return 0;
}

int lifecycle_validation_clear_and_isolation() {
    Fixture f;
    Material red(0xf800), blue(0x001f);
    red.shadingMode=blue.shadingMode=ShadingMode::UNLIT;
    Object a,b;
    addCenteredBox(a,-25,0,260,45,45,45,&red);
    addCenteredBox(b, 25,0,280,45,45,45,&blue);
    f.scene.addObject(&a); f.scene.addObject(&b);

    // Reference ordinary path before CSG is installed.
    f.frame();
    const std::vector<uint16_t> referenceColor=f.color;
    const std::vector<uint16_t> referenceDepth=f.depth;

    CsgDesc good;
    good.nodes={leaf(1),leaf(2),CsgNode::makeUnion(0,1)}; good.root=2;
    CsgBinding goodBindings[2]={{1,&a,-1},{2,&b,-1}};
    TASK_CHECK(f.scene.setCsgExpression(good,goodBindings,2).ok());
    TASK_CHECK(f.scene.csgExpressionActive());

    CsgDesc badChild;
    badChild.nodes={leaf(1),CsgNode::makeUnion(0,99)}; badChild.root=1;
    CsgStatus bad=f.scene.setCsgExpression(badChild,goodBindings,2);
    TASK_CHECK(bad.code==CsgError::ChildOutOfRange);
    TASK_CHECK(f.scene.csgExpressionActive());

    CsgDesc cycle;
    cycle.nodes={CsgNode::makeUnion(0,0)}; cycle.root=0;
    TASK_CHECK(f.scene.setCsgExpression(cycle,goodBindings,2).code==CsgError::Cycle);
    TASK_CHECK(f.scene.csgExpressionActive());

    CsgBinding duplicateDifferent[2]={{7,&a,-1},{7,&b,-1}};
    CsgDesc oneLeaf; oneLeaf.nodes={leaf(7)}; oneLeaf.root=0;
    TASK_CHECK(f.scene.setCsgExpression(oneLeaf,duplicateDifferent,2).code
               ==CsgError::DuplicateLeafBinding);

    CsgBinding badInstance{3,&a,99};
    TASK_CHECK(f.scene.setCsgExpression(oneLeaf,&badInstance,1).code
               ==CsgError::InstanceOutOfRange);
    TASK_CHECK(f.scene.csgExpressionActive());

    // Last-good expression still renders after all failed replacements.
    f.frame();
    TASK_CHECK(f.px(CX,CY) != 0);

    f.scene.clearCsgExpression();
    TASK_CHECK(!f.scene.csgExpressionActive());
    f.frame();
    TASK_CHECK(f.color==referenceColor);
    TASK_CHECK(f.depth==referenceDepth);

    // Unsupported leaf features are rejected before replacing configuration.
    Material gouraud(0xffff); gouraud.shadingMode=ShadingMode::GOURAUD;
    Object unsupported; addCenteredBox(unsupported,0,0,260,30,30,30,&gouraud);
    CsgBinding ub{1,&unsupported,-1};
    TASK_CHECK(f.scene.setCsgExpression(oneLeaf,&ub,1).code==CsgError::UnsupportedMaterial);
    Object billboard; billboard.isBillboard=true;
    CsgBinding bb{1,&billboard,-1};
    TASK_CHECK(f.scene.setCsgExpression(oneLeaf,&bb,1).code
               ==CsgError::UnsupportedObjectFeature);
    return 0;
}

int lifecycle_two_scenes_and_frames() {
    Fixture redScene, emptyScene;
    Material red(0xf800), green(0x07e0);
    red.shadingMode=green.shadingMode=ShadingMode::UNLIT;
    Object a,b;
    addCenteredBox(a,0,0,260,50,50,50,&red);
    addCenteredBox(b,0,0,260,50,50,50,&green);
    redScene.scene.addObject(&a);
    emptyScene.scene.addObject(&b);

    CsgDesc one; one.nodes={leaf(1)}; one.root=0;
    CsgBinding ab{1,&a,-1};
    TASK_CHECK(redScene.scene.setCsgExpression(one,&ab,1).ok());
    CsgDesc selfMinus;
    selfMinus.nodes={leaf(9),CsgNode::makeDifference(0,0)}; selfMinus.root=1;
    CsgBinding bb{9,&b,-1};
    TASK_CHECK(emptyScene.scene.setCsgExpression(selfMinus,&bb,1).ok());

    PickQuery q{CX,CY};
    redScene.scene.setPickQueries(&q,1);
    emptyScene.scene.setPickQueries(&q,1);
    redScene.frame();
    emptyScene.frame();
    TASK_CHECK(isRed(redScene.px(CX,CY)));
    TASK_CHECK(emptyScene.px(CX,CY)==0);
    TASK_CHECK(redScene.scene.getPickResults()[0].hit);
    TASK_CHECK(!emptyScene.scene.getPickResults()[0].hit);

    // Render the first Scene again; no stale second-Scene events or picks.
    redScene.frame();
    TASK_CHECK(isRed(redScene.px(CX,CY)));
    TASK_CHECK(redScene.scene.getPickResults()[0].object==&a);
    TASK_CHECK(!emptyScene.scene.getPickResults()[0].hit);

    // Ordinary Object rigid position also changes between prepareFrame calls.
    a.position = {150, 0, 0};
    redScene.frame();
    TASK_CHECK(redScene.px(CX,CY) == 0);
    return 0;
}

} // namespace

const struct task_case task_cases[] = {
    {"occupancy", "nested_reuse_self_empty", occupancy_nested_and_self},
    {"occupancy", "coplanar_contact_nonconvex_shell", occupancy_coplanar_and_nonconvex},
    {"occupancy", "cavity_near_and_far_planes", occupancy_cavity_near_far},
    {"boundary", "analytic_depth_and_stable_attribution", boundary_depth_tie_order},
    {"boundary", "directional_inner_wall_uv_occlusion", boundary_light_uv_occlusion},
    {"boundary", "shared_instances_and_serial_bands", boundary_shared_instances_bands},
    {"lifecycle", "validation_clear_exact_restore", lifecycle_validation_clear_and_isolation},
    {"lifecycle", "two_scenes_continuous_frames", lifecycle_two_scenes_and_frames},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);


