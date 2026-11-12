#include "ambient_occlusion.h"
#include <cmath>
#include <algorithm>

extern mat<4,4> Perspective, Viewport; // "OpenGL" state matrices, defined in our_gl.cpp

namespace {
constexpr double PI = 3.14159265358979323846;
}

void GBuffer::resize(const int width, const int height) {
    w = width;
    h = height;
    data.assign(static_cast<size_t>(w)*h, {});
}

void GBuffer::clear() {
    for (Entry &e : data) e = {};
}

void GBuffer::record(const int x, const int y, const vec3 &view_pos, const vec3 &view_nrm) {
    Entry *e = at(x, y);
    if (!e) return;
    const double n2 = view_nrm*view_nrm;
    if (!(n2 > 0) || !std::isfinite(n2)) { *e = {}; return; } // degenerate normal: no record
    e->pos   = view_pos;
    e->nrm   = view_nrm/std::sqrt(n2); // stored as a unit vector
    e->valid = true;
}

GBuffer::Entry* GBuffer::at(const int x, const int y) {
    if (x<0 || y<0 || x>=w || y>=h) return nullptr;
    return &data[x+y*w];
}

const GBuffer::Entry* GBuffer::at(const int x, const int y) const {
    if (x<0 || y<0 || x>=w || y>=h) return nullptr;
    return &data[x+y*w];
}

bool ao_params_valid(const AOParams &p) {
    return std::isfinite(p.radius) && p.radius > 0 &&
           std::isfinite(p.bias)   && p.bias   >= 0 &&
           p.filter_radius >= 0;
}

vec3 ao_sample_dir(const int j, const int k) {
    const double z   = (j+.5)/4.;
    const double phi = 2.*PI*(k + .5*(j%2))/4.;
    const double r   = std::sqrt(1.-z*z);
    return { r*std::cos(phi), r*std::sin(phi), z };
}

double ao_sample_dist(const AOParams &p, const int j) {
    return p.radius*(j+1)/4.;
}

void ao_tangent_frame(const vec3 &n, vec3 &t, vec3 &b) {
    // pick the coordinate axis least aligned with n for a stable frame
    vec3 axis = {1,0,0};
    if (std::abs(n.y) < std::abs(n*axis)) axis = {0,1,0};
    if (std::abs(n.z) < std::abs(n*axis)) axis = {0,0,1};
    t = normalized(cross(axis, n));
    b = cross(n, t);
}

bool compute_ao(const GBuffer &gb, const AOParams &params, std::vector<double> &ao_out) {
    if (!ao_params_valid(params)) return false;
    ao_out.assign(static_cast<size_t>(gb.w)*gb.h, 1.);
    for (int y=0; y<gb.h; y++) {
        for (int x=0; x<gb.w; x++) {
            const GBuffer::Entry *e0 = gb.at(x, y);
            if (!e0 || !e0->valid) continue; // background stays at 1
            vec3 t, b;
            ao_tangent_frame(e0->nrm, t, b);
            int occluded = 0;
            for (int j=0; j<4; j++) {
                const double dist = ao_sample_dist(params, j);
                for (int k=0; k<4; k++) {
                    const vec3 d = ao_sample_dir(j, k);
                    const vec3 s = e0->pos + (t*d.x + b*d.y + e0->nrm*d.z)*dist; // sample in view space
                    const vec4 clip = Perspective*vec4{s.x, s.y, s.z, 1.};       // back through the same camera
                    if (clip.w <= 0) continue;                                   // behind the camera: no occlusion
                    const vec4 scr = Viewport*vec4{clip.x/clip.w, clip.y/clip.w, clip.z/clip.w, 1.};
                    const GBuffer::Entry *e1 = gb.at(static_cast<int>(scr.x+.5), static_cast<int>(scr.y+.5));
                    if (!e1 || !e1->valid) continue;                             // off-screen or background
                    // occluded if the visible surface is closer to the camera than the sample
                    // by more than b in frontal depth (f - z, i.e. q.z > s.z + b),
                    // and within the sample radius of the center
                    if (e1->pos.z - s.z > params.bias && norm(e1->pos - s) <= params.radius)
                        occluded++;
                }
            }
            ao_out[x+y*gb.w] = 1. - occluded/static_cast<double>(AO_SAMPLE_COUNT);
        }
    }
    return true;
}

bool filter_ao(const GBuffer &gb, const AOParams &params, std::vector<double> &ao) {
    if (!ao_params_valid(params)) return false;
    if (ao.size() != static_cast<size_t>(gb.w)*gb.h) return false;
    const int r = params.filter_radius;
    if (r==0) return true; // filter disabled: keep the raw AO
    const double sig2 = 2.*(params.radius/4.)*(params.radius/4.);
    std::vector<double> tmp(ao.size(), 1.);
    for (int pass=0; pass<2; pass++) { // separable: horizontal then vertical
        const std::vector<double> &in  = pass ? tmp : ao;
        std::vector<double>       &out = pass ? ao  : tmp;
        for (int y=0; y<gb.h; y++) {
            for (int x=0; x<gb.w; x++) {
                const GBuffer::Entry *e0 = gb.at(x, y);
                if (!e0 || !e0->valid) { out[x+y*gb.w] = in[x+y*gb.w]; continue; } // background untouched
                double wsum = 0, asum = 0;
                for (int d=-r; d<=r; d++) {
                    const GBuffer::Entry *e1 = pass ? gb.at(x, y+d) : gb.at(x+d, y);
                    if (!e1 || !e1->valid) continue; // background skipped
                    const double dz = e0->pos.z - e1->pos.z; // frontal-depth difference (f cancels)
                    const double nd = std::max(0., e0->nrm*e1->nrm);
                    const double w  = std::exp(-static_cast<double>(d*d)/8.)
                                    * std::exp(-dz*dz/sig2)
                                    * std::pow(nd, 8.);
                    wsum += w;
                    asum += w*in[pass ? x+(y+d)*gb.w : (x+d)+y*gb.w];
                }
                out[x+y*gb.w] = wsum>0 ? asum/wsum : in[x+y*gb.w];
            }
        }
    }
    return true;
}

TGAImage ao_grayscale(const std::vector<double> &ao, const int w, const int h) {
    TGAImage img(w, h, TGAImage::GRAYSCALE);
    for (int y=0; y<h; y++)
        for (int x=0; x<w; x++) {
            const double v = std::clamp(ao[x+y*w], 0., 1.);
            const std::uint8_t g = static_cast<std::uint8_t>(std::lround(255.*v));
            img.set(x, y, {g, g, g, 255});
        }
    return img;
}
