#ifndef TEXTURE_H
#define TEXTURE_H
//==============================================================================================
// Originally written in 2016 by Peter Shirley <ptrshrl@gmail.com>
//
// To the extent possible under law, the author(s) have dedicated all copyright and related and
// neighboring rights to this software to the public domain worldwide. This software is
// distributed without any warranty.
//
// You should have received a copy (see file COPYING.txt) of the CC0 Public Domain Dedication
// along with this software. If not, see <http://creativecommons.org/publicdomain/zero/1.0/>.
//==============================================================================================

#include "perlin.h"
#include "rtw_stb_image.h"
#include "texture_footprint.h"

#include <cmath>
#include <stdexcept>
#include <vector>


class texture {
  public:
    virtual ~texture() = default;

    virtual color value(double u, double v, const point3& p) const = 0;

    // Footprint-aware query. The default implementation ignores the footprint
    // and delegates to the classic three-argument interface, so existing custom
    // textures keep working unchanged; mip_image_texture overrides this to
    // filter against the screen-space texture footprint.
    virtual color value(double u, double v, const point3& p,
                        const texture_footprint& footprint) const {
        return value(u, v, p);
    }
};


class solid_color : public texture {
  public:
    solid_color(const color& albedo) : albedo(albedo) {}

    solid_color(double red, double green, double blue) : solid_color(color(red,green,blue)) {}

    color value(double u, double v, const point3& p) const override {
        return albedo;
    }

  private:
    color albedo;
};


class checker_texture : public texture {
  public:
    checker_texture(double scale, shared_ptr<texture> even, shared_ptr<texture> odd)
      : inv_scale(1.0 / scale), even(even), odd(odd) {}

    checker_texture(double scale, const color& c1, const color& c2)
      : checker_texture(scale, make_shared<solid_color>(c1), make_shared<solid_color>(c2)) {}

    color value(double u, double v, const point3& p) const override {
        auto xInteger = int(std::floor(inv_scale * p.x()));
        auto yInteger = int(std::floor(inv_scale * p.y()));
        auto zInteger = int(std::floor(inv_scale * p.z()));

        bool isEven = (xInteger + yInteger + zInteger) % 2 == 0;

        return isEven ? even->value(u, v, p) : odd->value(u, v, p);
    }

  private:
    double inv_scale;
    shared_ptr<texture> even;
    shared_ptr<texture> odd;
};


class image_texture : public texture {
  public:
    image_texture(const char* filename) : image(filename) {}

    color value(double u, double v, const point3& p) const override {
        // If we have no texture data, then return solid cyan as a debugging aid.
        if (image.height() <= 0) return color(0,1,1);

        // Clamp input texture coordinates to [0,1] x [1,0]
        u = interval(0,1).clamp(u);
        v = 1.0 - interval(0,1).clamp(v);  // Flip V to image coordinates

        auto i = int(u * image.width());
        auto j = int(v * image.height());
        auto pixel = image.pixel_data(i,j);

        auto color_scale = 1.0 / 255.0;
        return color(color_scale*pixel[0], color_scale*pixel[1], color_scale*pixel[2]);
    }

  private:
    rtw_image image;
};


class mip_image_texture : public texture {
  public:
    // pixels are linear RGB, row-major with the first row on top; width and
    // height must be positive powers of two and may differ. The pixels are
    // copied, and the full mip pyramid is built at construction time.
    mip_image_texture(int width, int height, const std::vector<color>& pixels) {
        if (width <= 0 || height <= 0)
            throw std::invalid_argument("mip_image_texture: dimensions must be positive");
        if (!is_power_of_two(width) || !is_power_of_two(height))
            throw std::invalid_argument("mip_image_texture: dimensions must be powers of two");
        if (width > INT_WIDTH_MAX / height)
            throw std::invalid_argument("mip_image_texture: dimensions overflow");

        const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
        if (pixels.size() != count)
            throw std::invalid_argument("mip_image_texture: pixel count does not match dimensions");

        for (size_t i = 0; i < pixels.size(); i++) {
            const color& c = pixels[i];
            if (!std::isfinite(c.x()) || !std::isfinite(c.y()) || !std::isfinite(c.z())
                || c.x() < 0.0 || c.y() < 0.0 || c.z() < 0.0)
                throw std::invalid_argument("mip_image_texture: invalid pixel color");
        }

        level_widths.push_back(width);
        level_heights.push_back(height);
        levels.push_back(pixels);   // construction-time copy

        // Build successive levels as 2x2 linear means. Once an axis reaches one
        // texel, that axis repeats its single edge texel, down to 1x1.
        int w = width;
        int h = height;
        while (w > 1 || h > 1) {
            int nw = (w > 1) ? w / 2 : 1;
            int nh = (h > 1) ? h / 2 : 1;
            const std::vector<color>& src = levels.back();
            std::vector<color> dst(static_cast<size_t>(nw) * nh);

            for (int y = 0; y < nh; y++) {
                int y0 = 2*y;
                int y1 = (h > 1) ? y0 + 1 : y0;
                for (int x = 0; x < nw; x++) {
                    int x0 = 2*x;
                    int x1 = (w > 1) ? x0 + 1 : x0;
                    color mean = (src[index(w, x0, y0)] + src[index(w, x1, y0)]
                                + src[index(w, x0, y1)] + src[index(w, x1, y1)]) / 4.0;
                    dst[index(nw, x, y)] = mean;
                }
            }

            levels.push_back(dst);
            level_widths.push_back(nw);
            level_heights.push_back(nh);
            w = nw;
            h = nh;
        }
    }

    color value(double u, double v, const point3& p) const override {
        static const texture_footprint none;
        return value(u, v, p, none);
    }

    color value(double u, double v, const point3& p,
                const texture_footprint& footprint) const override {
        // A non-finite derivative is treated as a missing footprint, and never
        // reaches the level indexing math.
        if (!footprint.valid || !footprint.all_finite())
            return sample_level(0, u, v);

        // Footprint size in original (level-0) texels along both screen axes.
        const double w0 = static_cast<double>(level_widths[0]);
        const double h0 = static_cast<double>(level_heights[0]);
        double rho_x = std::sqrt((w0 * footprint.dudx) * (w0 * footprint.dudx)
                               + (h0 * footprint.dvdx) * (h0 * footprint.dvdx));
        double rho_y = std::sqrt((w0 * footprint.dudy) * (w0 * footprint.dudy)
                               + (h0 * footprint.dvdy) * (h0 * footprint.dvdy));
        double rho = std::fmax(1.0, std::fmax(rho_x, rho_y));
        if (!std::isfinite(rho))
            return sample_level(0, u, v);

        double lod = std::log(rho) / std::log(2.0);
        double max_lod = static_cast<double>(levels.size() - 1);
        if (lod < 0.0) lod = 0.0;
        if (lod > max_lod) lod = max_lod;

        int low = static_cast<int>(std::floor(lod));
        int high = std::min(low + 1, static_cast<int>(levels.size()) - 1);
        double blend = lod - static_cast<double>(low);

        color c0 = sample_level(low, u, v);
        color c1 = sample_level(high, u, v);
        return (1.0 - blend) * c0 + blend * c1;
    }

  private:
    // Pyramid levels; level 0 is the full-resolution copy.
    std::vector<std::vector<color>> levels;
    std::vector<int> level_widths;
    std::vector<int> level_heights;

    static const int INT_WIDTH_MAX = 2147483647;

    static bool is_power_of_two(int n) {
        return n > 0 && (n & (n - 1)) == 0;
    }

    static size_t index(int row_width, int x, int y) {
        return static_cast<size_t>(y) * static_cast<size_t>(row_width)
             + static_cast<size_t>(x);
    }

    static int wrap(int x, int size) {
        int r = x % size;          // U repeats around the seam.
        if (r < 0) r += size;
        return r;
    }

    static int clamp_index(int x, int size) {
        if (x < 0) return 0;       // V clamps to the edge.
        if (x > size - 1) return size - 1;
        return x;
    }

    color sample_level(int level, double u, double v) const {
        const int w = level_widths[level];
        const int h = level_heights[level];
        const std::vector<color>& data = levels[level];

        // Texel centers sit on half-integer coordinates. U wraps, V is flipped
        // (v grows upward while rows are stored top-first) and then clamped.
        double fx_coord = u * static_cast<double>(w) - 0.5;
        double fy_coord = (1.0 - v) * static_cast<double>(h) - 0.5;

        int x0 = static_cast<int>(std::floor(fx_coord));
        int y0 = static_cast<int>(std::floor(fy_coord));
        double tx = fx_coord - static_cast<double>(x0);
        double ty = fy_coord - static_cast<double>(y0);

        int x0w = wrap(x0, w);
        int x1w = wrap(x0 + 1, w);
        int y0c = clamp_index(y0, h);
        int y1c = clamp_index(y0 + 1, h);

        const color& c00 = data[index(w, x0w, y0c)];
        const color& c10 = data[index(w, x1w, y0c)];
        const color& c01 = data[index(w, x0w, y1c)];
        const color& c11 = data[index(w, x1w, y1c)];

        color top    = (1.0 - tx) * c00 + tx * c10;
        color bottom = (1.0 - tx) * c01 + tx * c11;
        return (1.0 - ty) * top + ty * bottom;
    }
};


class noise_texture : public texture {
  public:
    noise_texture(double scale) : scale(scale) {}

    color value(double u, double v, const point3& p) const override {
        return color(.5, .5, .5) * (1 + std::sin(scale * p.z() + 10 * noise.turb(p, 7)));
    }

  private:
    perlin noise;
    double scale;
};


#endif
