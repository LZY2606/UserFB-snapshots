//----------------------------------------------------------------------------
// Independent Porter-Duff reference for the AGG compositing regression.
//
// Everything here is computed in double precision, independently of the
// integer fixed-point paths in agg_pixfmt_rgba.h:
//
//   1. The four Porter-Duff operations are expressed on premultiplied,
//      normalized RGBA colors (W3C/SVG compositing formulas).
//   2. Geometric coverage (a cover byte in 0..255) interpolates between the
//      full-coverage result and the untouched destination.  Coverage is the
//      geometry coverage; the source alpha is never pre-scaled here.
//   3. Quantization to bytes happens once per operation, half-up, matching
//      rgba8::from_double (unsigned(v * 255 + 0.5)).
//
// The destination buffer in the tests always stores premultiplied values.
//----------------------------------------------------------------------------
#ifndef CHECKS_REFERENCE_H
#define CHECKS_REFERENCE_H

#include <vector>
#include <utility>
#include <cmath>
#include <cstdint>

namespace ref
{
    // Operation ids match agg::comp_op_e values used in the tests.
    enum op_e
    {
        op_src_over = 3,
        op_src_in   = 5,
        op_dst_out  = 8,
        op_src_atop = 9
    };

    inline const char* op_name(int op)
    {
        switch (op)
        {
        case op_src_over: return "source-over";
        case op_src_in:   return "source-in";
        case op_dst_out:  return "destination-out";
        case op_src_atop: return "source-atop";
        default:          return "unknown";
        }
    }

    // Premultiplied, normalized color (each channel in [0, 1]).
    struct fcolor
    {
        double r, g, b, a;
    };

    // Half-up byte quantization, identical in spirit to rgba8::from_double.
    inline int q8(double v)
    {
        if (v <= 0.0) return 0;
        if (v >= 1.0) return 255;
        return int(v * 255.0 + 0.5);
    }

    // Porter-Duff on premultiplied colors S (source) and D (destination).
    inline fcolor porter_duff(int op, const fcolor& s, const fcolor& d)
    {
        fcolor r;
        switch (op)
        {
        case op_src_over: // Co = S + D*(1 - Sa);  ao = Sa + Da*(1 - Sa)
            r.r = s.r + d.r * (1.0 - s.a);
            r.g = s.g + d.g * (1.0 - s.a);
            r.b = s.b + d.b * (1.0 - s.a);
            r.a = s.a + d.a * (1.0 - s.a);
            break;

        case op_src_in: // Co = S*Da;  ao = Sa*Da
            r.r = s.r * d.a;
            r.g = s.g * d.a;
            r.b = s.b * d.a;
            r.a = s.a * d.a;
            break;

        case op_dst_out: // Co = D*(1 - Sa);  ao = Da*(1 - Sa)
            r.r = d.r * (1.0 - s.a);
            r.g = d.g * (1.0 - s.a);
            r.b = d.b * (1.0 - s.a);
            r.a = d.a * (1.0 - s.a);
            break;

        case op_src_atop: // Co = S*Da + D*(1 - Sa);  ao = Da
            r.r = s.r * d.a + d.r * (1.0 - s.a);
            r.g = s.g * d.a + d.g * (1.0 - s.a);
            r.b = s.b * d.a + d.b * (1.0 - s.a);
            r.a = d.a;
            break;

        default:
            r.r = r.g = r.b = r.a = 0.0;
            break;
        }
        return r;
    }

    // Geometric cover interpolation: the covered fraction takes the full
    // operation result, the uncovered fraction keeps the original target.
    inline fcolor apply_cover(const fcolor& full, const fcolor& dst, int cover)
    {
        double t = cover / 255.0;
        fcolor r;
        r.r = full.r * t + dst.r * (1.0 - t);
        r.g = full.g * t + dst.g * (1.0 - t);
        r.b = full.b * t + dst.b * (1.0 - t);
        r.a = full.a * t + dst.a * (1.0 - t);
        return r;
    }

    struct bgra_order
    {
        static const int R = 2, G = 1, B = 0, A = 3;
    };
    struct rgba_order
    {
        static const int R = 0, G = 1, B = 2, A = 3;
    };

    // One independently referenced compositing step on byte RGBA storage.
    //
    // src[]/dst[] are indexed with the given byte order.  dst is always
    // premultiplied; src is premultiplied when src_is_premultiplied, plain
    // (straight) otherwise.  Writes the four expected bytes into out[].
    template<class Order>
    inline void compose_byte(int op,
                             const unsigned char src[4],
                             const unsigned char dst[4],
                             int cover,
                             bool src_is_premultiplied,
                             unsigned char out[4])
    {
        fcolor s;
        double sa = src[Order::A] / 255.0;
        if (src_is_premultiplied)
        {
            s.r = src[Order::R] / 255.0;
            s.g = src[Order::G] / 255.0;
            s.b = src[Order::B] / 255.0;
            s.a = sa;
        }
        else
        {
            // Premultiply the straight source in exact double arithmetic.
            s.r = (src[Order::R] / 255.0) * sa;
            s.g = (src[Order::G] / 255.0) * sa;
            s.b = (src[Order::B] / 255.0) * sa;
            s.a = sa;
        }

        fcolor d;
        d.r = dst[Order::R] / 255.0;
        d.g = dst[Order::G] / 255.0;
        d.b = dst[Order::B] / 255.0;
        d.a = dst[Order::A] / 255.0;

        fcolor full = porter_duff(op, s, d);
        fcolor got  = apply_cover(full, d, cover);

        out[Order::R] = (unsigned char)q8(got.r);
        out[Order::G] = (unsigned char)q8(got.g);
        out[Order::B] = (unsigned char)q8(got.b);
        out[Order::A] = (unsigned char)q8(got.a);
    }

    //-------------------------------------------------------------- geometry
    struct dpoint
    {
        double x, y;
    };

    typedef std::vector<dpoint> polygon;

    inline double cross(const dpoint& a, const dpoint& b, const dpoint& p)
    {
        return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
    }

    inline dpoint intersect(const dpoint& p, const dpoint& q,
                            const dpoint& a, const dpoint& b)
    {
        double den = (p.x - q.x) * (a.y - b.y) - (p.y - q.y) * (a.x - b.x);
        double t = ((p.x - a.x) * (a.y - b.y) -
                    (p.y - a.y) * (a.x - b.x)) / den;
        dpoint r;
        r.x = p.x + t * (q.x - p.x);
        r.y = p.y + t * (q.y - p.y);
        return r;
    }

    // Sutherland-Hodgman clipping of a polygon against a pixel square.
    inline polygon clip_to_cell(const polygon& poly, int cx, int cy)
    {
        dpoint corners[4] =
        {
            { double(cx),     double(cy)     },
            { double(cx + 1), double(cy)     },
            { double(cx + 1), double(cy + 1) },
            { double(cx),     double(cy + 1) }
        };

        polygon out = poly;
        for (int e = 0; e < 4; ++e)
        {
            const dpoint& a = corners[e];
            const dpoint& b = corners[(e + 1) % 4];
            polygon in = out;
            out.clear();
            if (in.empty()) continue;

            dpoint s = in.back();
            bool s_in = cross(a, b, s) >= -1e-12;
            for (size_t i = 0; i < in.size(); ++i)
            {
                const dpoint& p = in[i];
                bool p_in = cross(a, b, p) >= -1e-12;
                if (p_in)
                {
                    if (!s_in) out.push_back(intersect(s, p, a, b));
                    out.push_back(p);
                }
                else if (s_in)
                {
                    out.push_back(intersect(s, p, a, b));
                }
                s = p;
                s_in = p_in;
            }
        }
        return out;
    }

    inline double polygon_area(const polygon& poly)
    {
        if (poly.size() < 3) return 0.0;
        double sum = 0.0;
        for (size_t i = 0; i < poly.size(); ++i)
        {
            const dpoint& p = poly[i];
            const dpoint& q = poly[(i + 1) % poly.size()];
            sum += p.x * q.y - q.x * p.y;
        }
        return std::fabs(sum) * 0.5;
    }

    // Exact fraction of pixel cell (cx, cy) covered by the polygon.
    inline double cell_coverage(const polygon& poly, int cx, int cy)
    {
        polygon clipped = clip_to_cell(poly, cx, cy);
        double a = polygon_area(clipped);
        if (a < 0.0) a = 0.0;
        if (a > 1.0) a = 1.0;
        return a;
    }
}

#endif
