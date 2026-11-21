#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "Image.hpp"
#include "../utilities/RGBColor.hpp"
#include "../world/ViewPlane.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

Image::Image(int h, int v) : hres(h), vres(v) {
    colors = new RGBColor*[hres];
    for (int i = 0; i < hres; i++)
        colors[i] = new RGBColor[vres];
}

Image::Image(const ViewPlane &vp) : Image(vp.hres, vp.vres) {}

Image::~Image() {
    for (int i = 0; i < hres; i++) delete[] colors[i];
    delete[] colors;
}

void Image::set_pixel(int x, int y, const RGBColor &color) {
    colors[x][y] = color;
}

void Image::write_png(std::string path) const {
    std::vector<unsigned char> buf(hres * vres * 3);
    for (int y = 0; y < vres; y++) {
        for (int x = 0; x < hres; x++) {
            int idx = (y * hres + x) * 3;
            // clamp here at output only
            float cr = std::max(0.0f, std::min(1.0f, colors[x][y].r));
            float cg = std::max(0.0f, std::min(1.0f, colors[x][y].g));
            float cb = std::max(0.0f, std::min(1.0f, colors[x][y].b));
            buf[idx + 0] = (unsigned char)(cr * 255.0f);
            buf[idx + 1] = (unsigned char)(cg * 255.0f);
            buf[idx + 2] = (unsigned char)(cb * 255.0f);
        }
    }
    stbi_write_png(path.c_str(), hres, vres, 3, buf.data(), hres * 3);
}
