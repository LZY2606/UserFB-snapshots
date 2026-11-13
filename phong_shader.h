#pragma once
// Shared Phong lighting: the original main.cpp shader and the shadow-volume
// path use the exact same UV, tangent-space normal, diffuse and specular
// computations. The legacy path quantizes to bytes immediately (preserving its
// historical output bit for bit); the shadow path keeps ambient and
// diffuse+specular unquantized until final composition.
#include <algorithm>
#include <cmath>
#include "geometry.h"
#include "tgaimage.h"
#include "model.h"
#include "our_gl.h"

extern mat<4,4> ModelView, Perspective; // "OpenGL" state matrices

struct PhongTerms {
    vec2  uv;
    vec4  n;            // tangent-mapped normal in eye space
    vec4  r;            // reflected light direction in eye space
    double ambient;
    double diffuse;
    double specular;
};

// Core evaluation shared by both shaders. `bar` are perspective-correct
// barycentric weights of the original three vertices.
inline PhongTerms phong_evaluate(const Model &model, const vec4 &l,
                                 const vec4 tri[3], const vec2 varying_uv[3],
                                 const vec4 varying_nrm[3], const vec3 &bar) {
    mat<2,4> E = { tri[1]-tri[0], tri[2]-tri[0] };
    mat<2,2> U = { varying_uv[1]-varying_uv[0], varying_uv[2]-varying_uv[0] };
    mat<2,4> T = U.invert() * E;
    mat<4,4> D = {normalized(T[0]),  // tangent vector
                  normalized(T[1]),  // bitangent vector
                  normalized(varying_nrm[0]*bar[0] + varying_nrm[1]*bar[1] + varying_nrm[2]*bar[2]),
                  {0,0,0,1}}; // Darboux frame
    PhongTerms t;
    t.uv = varying_uv[0] * bar[0] + varying_uv[1] * bar[1] + varying_uv[2] * bar[2];
    t.n = normalized(D.transpose() * model.normal(t.uv));
    t.r = normalized(t.n * (t.n * l)*2 - l);
    t.ambient  = .4;
    t.diffuse  = 1.*std::max(0., t.n * l);
    t.specular = (.5+2.*IShader::sample2D(model.specular(), t.uv)[0]/255.) * std::pow(std::max(t.r.z, 0.), 35);
    return t;
}

// Legacy shader used by main.cpp; output is quantized in the fragment exactly
// as the original tinyrenderer shader did.
struct PhongShader : IShader {
    const Model &model;
    vec4 l;              // light direction in eye coordinates
    vec2  varying_uv[3];
    vec4 varying_nrm[3];
    vec4 tri[3];         // triangle in view coordinates

    PhongShader(const vec3 light, const Model &m) : model(m) {
        l = normalized((ModelView*vec4{light.x, light.y, light.z, 0.}));
    }

    virtual vec4 vertex(const int face, const int vert) {
        varying_uv[vert]  = model.uv(face, vert);
        varying_nrm[vert] = ModelView.invert_transpose() * model.normal(face, vert);
        vec4 gl_Position = ModelView * model.vert(face, vert);
        tri[vert] = gl_Position;
        return Perspective * gl_Position; // in clip coordinates
    }

    virtual std::pair<bool,TGAColor> fragment(const vec3 bar) const {
        PhongTerms t = phong_evaluate(model, l, tri, varying_uv, varying_nrm, bar);
        TGAColor gl_FragColor = IShader::sample2D(model.diffuse(), t.uv);
        for (int channel : {0,1,2})
            gl_FragColor[channel] = std::min<int>(255, gl_FragColor[channel]*(t.ambient + t.diffuse + t.specular));
        return {false, gl_FragColor};
    }
};

// Shadow-path shader: records unquantized albedo, ambient and direct light,
// reusing phong_evaluate() verbatim (same UV, tangent normal, diffuse and
// specular math as the legacy shader).
struct ShadowPhongShader : ISurfaceShader {
    const Model &model;
    vec4 l;
    vec2 varying_uv[3];
    vec4 varying_nrm[3];
    vec4 tri[3];

    ShadowPhongShader(const vec3 light, const Model &m) : model(m) {
        l = normalized((ModelView*vec4{light.x, light.y, light.z, 0.}));
    }

    SurfaceVaryings vertex(const int face, const int nthvert, vec4 &clip) override {
        varying_uv[nthvert]  = model.uv(face, nthvert);
        varying_nrm[nthvert] = ModelView.invert_transpose() * model.normal(face, nthvert);
        tri[nthvert]         = ModelView * model.vert(face, nthvert);
        clip                 = Perspective * tri[nthvert];
        SurfaceVaryings v;
        v.uv   = varying_uv[nthvert];
        v.nrm  = varying_nrm[nthvert];
        v.view = tri[nthvert];
        return v;
    }

    SurfaceFragment fragment(const SurfaceVaryings /*corners*/[3],
                             const vec3 &bar) const override {
        PhongTerms t = phong_evaluate(model, l, tri, varying_uv, varying_nrm, bar);
        TGAColor tex = IShader::sample2D(model.diffuse(), t.uv);
        SurfaceFragment f;
        f.albedo  = vec3{(double)tex[0], (double)tex[1], (double)tex[2]};
        f.ambient = t.ambient;
        f.direct  = t.diffuse + t.specular;
        return f;
    }
};
