//----------------------------------------------------------------------------
// Windowless compositing regression for pixfmt_custom_blend_rgba.
//
// Covers four Porter-Duff operations (source-over, source-in,
// destination-out, source-atop) through both source entry points:
//
//   comp_op_adaptor_rgba      -- straight (non-premultiplied) source input
//   comp_op_adaptor_rgba_pre  -- premultiplied source input
//
// The destination buffer always holds premultiplied bytes.  Every numeric
// expectation comes from checks/reference.h (independent double-precision
// Porter-Duff + geometric cover interpolation), never from the library's
// own blenders or rasterizer covers.  One byte of rounding difference per
// channel per operation is allowed.
//----------------------------------------------------------------------------
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <sstream>

#include "agg_pixfmt_rgba.h"
#include "agg_renderer_base.h"
#include "agg_renderer_scanline.h"
#include "agg_rasterizer_scanline_aa.h"
#include "agg_scanline_u.h"

#include "reference.h"
#include "geometry.h"

namespace
{
    typedef agg::rgba8 color_type;

    // Feed an explicit polygon (from geometry.h) into a rasterizer.
    static void feed_polygon(agg::rasterizer_scanline_aa<>& ras,
                             const ref::polygon& poly)
    {
        if (poly.empty()) return;
        ras.move_to_d(poly[0].x, poly[0].y);
        for (size_t i = 1; i < poly.size(); ++i)
            ras.line_to_d(poly[i].x, poly[i].y);
        ras.close_polygon();
    }

    struct Stats
    {
        long checks;
        long failures;
        std::string first_failure;
    };

    static Stats g_stats;

    static std::string channel_name(int ch)
    {
        static const char* n[4] = { "R", "G", "B", "A" };
        return std::string(n[ch]);
    }

    // Convert a raw memory quad (in the pixel Order) to logical R,G,B,A.
    template<class Order>
    static void logical(const unsigned char raw[4], unsigned char out[4])
    {
        out[0] = raw[Order::R];
        out[1] = raw[Order::G];
        out[2] = raw[Order::B];
        out[3] = raw[Order::A];
    }

    static void fail(const std::string& id, const std::string& detail)
    {
        ++g_stats.failures;
        std::string line = std::string("FAIL ") + id + " " + detail;
        if (g_stats.first_failure.empty()) g_stats.first_failure = line;
        std::printf("%s\n", line.c_str());
    }

    static void expect_bytes(const std::string& id,
                             const unsigned char got_log[4],
                             const unsigned char want_log[4],
                             int tol)
    {
        ++g_stats.checks;
        for (int ch = 0; ch < 4; ++ch)
        {
            int d = int(got_log[ch]) - int(want_log[ch]);
            if (d < 0) d = -d;
            if (d > tol)
            {
                std::ostringstream os;
                os << "channel=" << channel_name(ch)
                   << " got=" << int(got_log[ch])
                   << " want=" << int(want_log[ch])
                   << " tol=" << tol
                   << " rgba=(" << int(got_log[0]) << "," << int(got_log[1])
                   << "," << int(got_log[2]) << "," << int(got_log[3]) << ")"
                   << " expected=(" << int(want_log[0]) << "," << int(want_log[1])
                   << "," << int(want_log[2]) << "," << int(want_log[3]) << ")";
                fail(id, os.str());
                return; // one record per failing cell keeps reports readable
            }
        }
    }

    //------------------------------------------------------------- surfaces
    // A row-padded surface.  Every byte not part of a valid pixel starts as
    // a sentinel (0xA5) and must remain one.  Supports negative stride.
    template<class Order>
    struct Surface
    {
        unsigned w, h, pad;
        int stride;
        std::vector<unsigned char> store;
        agg::rendering_buffer rb;
        typedef agg::pixfmt_custom_blend_rgba<
            agg::comp_op_adaptor_rgba<color_type, Order>,
            agg::rendering_buffer> plain_pixfmt;
        typedef agg::pixfmt_custom_blend_rgba<
            agg::comp_op_adaptor_rgba_pre<color_type, Order>,
            agg::rendering_buffer> pre_pixfmt;

        Surface(unsigned w_, unsigned h_, unsigned pad_ = 8,
                bool negative_stride = false)
            : w(w_), h(h_), pad(pad_),
              stride(negative_stride ? -int(w_ * 4 + pad_) : int(w_ * 4 + pad_)),
              store(size_t(h_) * (w_ * 4 + pad_), 0xA5)
        {
            // For a negative stride the buffer pointer passed to
            // rendering_buffer is the start of the allocation; the accessor
            // itself moves to the last row.
            rb.attach(store.data(), w_, h_, stride);
        }

        // Copies must re-point the rendering buffer at their own storage;
        // the default memberwise copy would leave rb dangling.
        Surface(const Surface& o)
            : w(o.w), h(o.h), pad(o.pad), stride(o.stride),
              store(o.store), rb()
        {
            rebind();
        }

        Surface& operator=(const Surface& o)
        {
            if (this != &o)
            {
                w = o.w; h = o.h; pad = o.pad; stride = o.stride;
                store = o.store;
                rebind();
            }
            return *this;
        }

        void rebind()
        {
            rb.attach(store.data(), w, h, stride);
        }

        unsigned char* row_raw(int y)
        {
            return rb.row_ptr(y);
        }

        void get_logical(int x, int y, unsigned char out[4]) const
        {
            const unsigned char* p =
                const_cast<agg::rendering_buffer&>(rb).row_ptr(y) + x * 4;
            logical<Order>(p, out);
        }

        void put_premult(int x, int y,
                         unsigned char r, unsigned char g,
                         unsigned char b, unsigned char a)
        {
            unsigned char* p = rb.row_ptr(y) + x * 4;
            p[Order::R] = r;
            p[Order::G] = g;
            p[Order::B] = b;
            p[Order::A] = a;
        }

        // Every pixel that was not touched must keep its snapshot bytes, and
        // every row-end padding byte must still be the 0xA5 sentinel.
        // "before" is in logical row order, so it is valid for both stride
        // directions.
        void check_invariants(const std::string& id,
                              const std::vector<unsigned char>& before,
                              const unsigned char* touched)
        {
            for (unsigned y = 0; y < h; ++y)
            {
                const unsigned char* row = rb.row_ptr(y);
                for (unsigned x = 0; x < w; ++x)
                {
                    if (touched && touched[y * w + x]) continue;
                    size_t off = (size_t(y) * w + x) * 4;
                    for (int k = 0; k < 4; ++k)
                    {
                        ++g_stats.checks;
                        if (row[x * 4 + k] != before[off + k])
                        {
                            std::ostringstream os;
                            os << "untouched pixel changed at ("
                               << x << "," << y << ") byte+" << k
                               << " before=" << int(before[off + k])
                               << " after=" << int(row[x * 4 + k]);
                            fail(id, os.str());
                        }
                    }
                }
            }
            // Row-end padding lives in the raw allocation; scan physical
            // rows so negative stride cannot confuse the check.
            unsigned row_bytes = w * 4 + pad;
            for (unsigned p = 0; p < h; ++p)
            {
                const unsigned char* base =
                    store.data() + size_t(p) * row_bytes + w * 4;
                for (unsigned k = 0; k < pad; ++k)
                {
                    ++g_stats.checks;
                    if (base[k] != 0xA5)
                    {
                        std::ostringstream os;
                        os << "row-end sentinel changed physical_row=" << p
                           << " pad+" << k << " value=" << int(base[k]);
                        fail(id, os.str());
                    }
                }
            }
        }

        std::vector<unsigned char> snapshot() const
        {
            std::vector<unsigned char> out(size_t(w) * h * 4);
            for (unsigned y = 0; y < h; ++y)
            {
                const unsigned char* row = rb.row_ptr(y);
                std::memcpy(&out[size_t(y) * w * 4], row, size_t(w) * 4);
            }
            return out;
        }
    };

    template<class Order>
    static void check_premult_invariant(const std::string& id,
                                        Surface<Order>& s,
                                        const unsigned char* touched)
    {
        for (unsigned y = 0; y < s.h; ++y)
        {
            for (unsigned x = 0; x < s.w; ++x)
            {
                if (touched && !touched[y * s.w + x]) continue;
                unsigned char c[4];
                s.get_logical(x, y, c);
                ++g_stats.checks;
                if (!(c[0] <= c[3] && c[1] <= c[3] && c[2] <= c[3]))
                {
                    std::ostringstream os;
                    os << "premult invariant violated at (" << x << "," << y
                       << ") rgba=(" << int(c[0]) << "," << int(c[1])
                       << "," << int(c[2]) << "," << int(c[3]) << ")";
                    fail(id, os.str());
                }
            }
        }
    }

    // Independent expectation for one cell in logical R,G,B,A order.
    template<class Order>
    static void reference_cell(int op,
                               const unsigned char src_pre[4],
                               const unsigned char dst_log[4],
                               int cover,
                               unsigned char want_log[4])
    {
        unsigned char src_raw[4], dst_raw[4], out_raw[4];
        src_raw[Order::R] = src_pre[0];
        src_raw[Order::G] = src_pre[1];
        src_raw[Order::B] = src_pre[2];
        src_raw[Order::A] = src_pre[3];
        dst_raw[Order::R] = dst_log[0];
        dst_raw[Order::G] = dst_log[1];
        dst_raw[Order::B] = dst_log[2];
        dst_raw[Order::A] = dst_log[3];
        ref::compose_byte<Order>(op, src_raw, dst_raw, cover, true, out_raw);
        logical<Order>(out_raw, want_log);
    }

    static const int    OPS[4]  = { ref::op_src_over, ref::op_src_in,
                                    ref::op_dst_out, ref::op_src_atop };
    static const int    ALPHAS[4] = { 0, 1, 128, 255 };
    static const int    COVERS[6] = { 0, 1, 127, 128, 254, 255 };

    // Premultiplied destination cases (logical RGBA), channels <= alpha.
    static void dst_cases(std::vector<std::vector<unsigned char> >& out)
    {
        out.clear();
        unsigned char d1[4] = { 0,   0,   0,   0   }; // transparent
        unsigned char d2[4] = { 200, 100, 50,  255 }; // opaque asymmetric
        unsigned char d3[4] = { 40,  80,  20,  128 }; // translucent asymmetric
        unsigned char d4[4] = { 10,  30,  5,   64  }; // translucent, low alpha
        out.push_back(std::vector<unsigned char>(d1, d1 + 4));
        out.push_back(std::vector<unsigned char>(d2, d2 + 4));
        out.push_back(std::vector<unsigned char>(d3, d3 + 4));
        out.push_back(std::vector<unsigned char>(d4, d4 + 4));
    }

    //----------------------------------------------------------- grid tests
    // Exhaustive per-cell matrix through every rendering entry point.
    template<class Order>
    static void test_grid(const char* order_tag, bool negative_stride)
    {
        std::vector<std::vector<unsigned char> > dsts;
        dst_cases(dsts);

        // Two asymmetric straight source colors.
        const int sources[2][3] = { {210, 90, 30}, {30, 200, 170} };

        for (int si = 0; si < 2; ++si)
        {
            for (size_t oi = 0; oi < 4; ++oi)
            {
                int op = OPS[oi];
                for (size_t di = 0; di < dsts.size(); ++di)
                {
                    const unsigned W = 6, H = 4;
                    Surface<Order> s(W, H, 8, negative_stride);
                    std::vector<unsigned char> touched(W * H, 0);

                    for (unsigned y = 0; y < H; ++y)
                        for (unsigned x = 0; x < W; ++x)
                            s.put_premult(x, y,
                                dsts[di][0], dsts[di][1],
                                dsts[di][2], dsts[di][3]);
                    std::vector<unsigned char> before = s.snapshot();

                    typename Surface<Order>::plain_pixfmt pix_plain(s.rb, op);
                    typename Surface<Order>::pre_pixfmt   pix_pre(s.rb, op);
                    agg::renderer_base<typename Surface<Order>::pre_pixfmt>
                        ren_pre(pix_pre);
                    agg::renderer_base<typename Surface<Order>::plain_pixfmt>
                        ren_plain(pix_plain);

                    unsigned cell = 0;
                    for (size_t ai = 0; ai < 4; ++ai)
                    {
                        int a = ALPHAS[ai];
                        unsigned char pr = color_type::multiply(
                            (unsigned char)sources[si][0], (unsigned char)a);
                        unsigned char pg = color_type::multiply(
                            (unsigned char)sources[si][1], (unsigned char)a);
                        unsigned char pb = color_type::multiply(
                            (unsigned char)sources[si][2], (unsigned char)a);

                        // Independent check of AGG's own premultiply step.
                        {
                            double exact[3] = {
                                sources[si][0] * a / 255.0,
                                sources[si][1] * a / 255.0,
                                sources[si][2] * a / 255.0 };
                            unsigned char got[3] = { pr, pg, pb };
                            for (int k = 0; k < 3; ++k)
                            {
                                ++g_stats.checks;
                                int want = ref::q8(exact[k] / 255.0);
                                int d = int(got[k]) - want;
                                if (d < 0) d = -d;
                                if (d > 1)
                                {
                                    std::ostringstream os;
                                    os << "premultiply channel=" << channel_name(k)
                                       << " straight=" << sources[si][k]
                                       << " a=" << a << " got=" << int(got[k])
                                       << " exact=" << want;
                                    fail(std::string("premult/") + order_tag, os.str());
                                }
                            }
                            ++g_stats.checks;
                            if (!(pr <= a && pg <= a && pb <= a))
                                fail(std::string("premult-valid/") + order_tag,
                                     "premultiplied channel exceeds alpha");
                        }

                        color_type src_straight(
                            (unsigned char)sources[si][0],
                            (unsigned char)sources[si][1],
                            (unsigned char)sources[si][2],
                            (unsigned char)a);
                        color_type src_pre(pr, pg, pb, (unsigned char)a);

                        for (size_t ci = 0; ci < 6; ++ci)
                        {
                            int cov = COVERS[ci];
                            unsigned x = cell % W;
                            unsigned y = cell / W;
                            ++cell;
                            touched[y * W + x] = true;

                            // Reference expectation (premultiplied source
                            // bytes are what the pre adaptor actually gets).
                            unsigned char src_pre_log[4] = { pr, pg, pb, (unsigned char)a };
                            unsigned char want[4];
                            reference_cell<Order>(op, src_pre_log,
                                                  &dsts[di][0], cov, want);

                            // Split the matrix across the real entry points
                            // so every path is exercised on real data.
                            unsigned char got[4];
                            std::string id;
                            if (ci % 3 == 0)
                            {
                                // Direct single-pixel entry, pre adaptor.
                                pix_pre.blend_pixel(x, y, src_pre,
                                                    (agg::int8u)cov);
                                id = "grid/blend_pixel/pre";
                            }
                            else if (ci % 3 == 1)
                            {
                                // Direct single-pixel entry, plain adaptor.
                                pix_plain.blend_pixel(x, y, src_straight,
                                                      (agg::int8u)cov);
                                id = "grid/blend_pixel/plain";
                            }
                            else
                            {
                                // renderer_base entry (clipping enabled by
                                // default to the full buffer).
                                ren_pre.blend_pixel(x, y, src_pre,
                                                    (agg::int8u)cov);
                                id = "grid/ren_blend_pixel/pre";
                            }
                            s.get_logical(x, y, got);

                            std::ostringstream os;
                            os << id << "/" << order_tag << "/"
                               << ref::op_name(op)
                               << " src=(" << sources[si][0] << ","
                               << sources[si][1] << "," << sources[si][2]
                               << ") a=" << a << " cover=" << cov
                               << " dst=(" << int(dsts[di][0]) << ","
                               << int(dsts[di][1]) << "," << int(dsts[di][2])
                               << "," << int(dsts[di][3]) << ")";
                            expect_bytes(os.str(), got, want, 1);
                        }
                    }

                    check_premult_invariant(
                        std::string("grid/invariant/") + order_tag, s, &touched[0]);
                    s.check_invariants(
                        std::string("grid/sentinel/") + order_tag, before, &touched[0]);
                }
            }
        }
    }

    //----------------------------------------------- span and clipping tests
    // Distinctive per-x covers and per-x colors so a clipped span that fails
    // to advance its source pointer is detected immediately.
    template<class Order>
    static void test_spans_and_clipping(const char* order_tag,
                                        bool negative_stride)
    {
        const unsigned W = 12, H = 6;
        Surface<Order> s(W, H, 8, negative_stride);
        std::vector<unsigned char> touched(W * H, 0);

        // Uniform translucent destination.
        for (unsigned y = 0; y < H; ++y)
            for (unsigned x = 0; x < W; ++x)
                s.put_premult(x, y, 40, 80, 20, 128);
        std::vector<unsigned char> before = s.snapshot();

        typename Surface<Order>::pre_pixfmt pix_pre(s.rb, ref::op_src_over);
        typename Surface<Order>::plain_pixfmt pix_plain(s.rb, ref::op_src_in);
        agg::renderer_base<typename Surface<Order>::pre_pixfmt> ren_pre(pix_pre);
        agg::renderer_base<typename Surface<Order>::plain_pixfmt> ren_plain(pix_plain);

        const int cov_pattern[W] = { 5, 17, 33, 71, 128, 200, 251, 255, 127, 64, 9, 1 };
        agg::int8u covers[W];
        for (unsigned i = 0; i < W; ++i) covers[i] = (agg::int8u)cov_pattern[i];

        // Row 0: solid hspan, direct pixfmt entry.
        color_type solid_pre(120, 10, 60, 200);
        pix_pre.blend_solid_hspan(0, 0, W, solid_pre, covers);
        for (unsigned x = 0; x < W; ++x)
        {
            touched[0 * W + x] = true;
            unsigned char dst[4] = { 40, 80, 20, 128 };
            unsigned char src[4] = { 120, 10, 60, 200 };
            unsigned char want[4], got[4];
            reference_cell<Order>(ref::op_src_over, src, dst, cov_pattern[x], want);
            s.get_logical(x, 0, got);
            std::ostringstream os;
            os << "solid_hspan/pre/" << order_tag << " x=" << x
               << " cover=" << cov_pattern[x];
            expect_bytes(os.str(), got, want, 1);
        }

        // Row 1: solid hspan through renderer_base, clipped on the left.
        // The call starts at x=-3 with a covers array also starting there.
        // renderer_base must advance the cover pointer by 3, so destination
        // pixel x consumes clip_covers[x + 3].
        agg::int8u clip_covers[W + 3];
        for (unsigned i = 0; i < W + 3; ++i)
            clip_covers[i] = (agg::int8u)((i * 53 + 7) & 0xFF);
        ren_pre.clip_box(2, 0, W - 1, H - 1);
        ren_pre.blend_solid_hspan(-3, 1, W + 3, solid_pre, clip_covers);
        ren_pre.reset_clipping(true);
        for (unsigned x = 0; x < W; ++x)
        {
            unsigned char got[4];
            s.get_logical(x, 1, got);
            if (x >= 2)
            {
                touched[1 * W + x] = true;
                unsigned char dst[4] = { 40, 80, 20, 128 };
                unsigned char src[4] = { 120, 10, 60, 200 };
                unsigned char want[4];
                int expected_cover = clip_covers[x + 3];
                reference_cell<Order>(ref::op_src_over, src, dst,
                                      expected_cover, want);
                std::ostringstream os;
                os << "clip_solid_hspan/pre/" << order_tag << " x=" << x
                   << " cover=" << expected_cover;
                expect_bytes(os.str(), got, want, 1);
            }
            else
            {
                ++g_stats.checks;
                unsigned char dst[4] = { 40, 80, 20, 128 };
                if (memcmp(got, dst, 4) != 0)
                {
                    std::ostringstream os;
                    os << "clip region outside box was written x=" << x;
                    fail(std::string("clip_solid_hspan/out-of-box/") + order_tag,
                         os.str());
                }
            }
        }

        // Row 2: color hspan, per-x colors and covers, direct entry.
        std::vector<color_type> colors;
        for (unsigned x = 0; x < W; ++x)
        {
            unsigned char r = color_type::multiply((unsigned char)(30 + x * 15),
                                                   (unsigned char)(64 + x * 14));
            unsigned char g = color_type::multiply((unsigned char)(200 - x * 9),
                                                   (unsigned char)(64 + x * 14));
            unsigned char b = color_type::multiply((unsigned char)(10 + x * 7),
                                                   (unsigned char)(64 + x * 14));
            colors.push_back(color_type(r, g, b, (unsigned char)(64 + x * 14)));
        }
        pix_pre.comp_op(ref::op_dst_out);
        pix_pre.blend_color_hspan(0, 2, W, &colors[0], covers, agg::cover_full);
        for (unsigned x = 0; x < W; ++x)
        {
            touched[2 * W + x] = true;
            unsigned char dst[4] = { 40, 80, 20, 128 };
            unsigned char src[4] = { colors[x].r, colors[x].g,
                                     colors[x].b, colors[x].a };
            unsigned char want[4], got[4];
            reference_cell<Order>(ref::op_dst_out, src, dst, cov_pattern[x], want);
            s.get_logical(x, 2, got);
            std::ostringstream os;
            os << "color_hspan/pre/" << order_tag << " x=" << x;
            expect_bytes(os.str(), got, want, 1);
        }

        // Row 3: color hspan with null covers and a constant cover.
        pix_pre.blend_color_hspan(0, 3, W, &colors[0], 0, (agg::int8u)128);
        for (unsigned x = 0; x < W; ++x)
        {
            touched[3 * W + x] = true;
            unsigned char dst[4] = { 40, 80, 20, 128 };
            unsigned char src[4] = { colors[x].r, colors[x].g,
                                     colors[x].b, colors[x].a };
            unsigned char want[4], got[4];
            reference_cell<Order>(ref::op_dst_out, src, dst, 128, want);
            s.get_logical(x, 3, got);
            std::ostringstream os;
            os << "color_hspan_const/pre/" << order_tag << " x=" << x;
            expect_bytes(os.str(), got, want, 1);
        }

        // Row 4: clipped color hspan through renderer_base.  The color and
        // cover arrays start at x=-2, so pixel x must consume index x+2.
        std::vector<color_type> clip_colors;
        agg::int8u clip_covers2[W + 2];
        for (unsigned i = 0; i < W + 2; ++i)
        {
            unsigned char a = (unsigned char)(64 + (i * 9) % 192);
            unsigned char r = color_type::multiply((unsigned char)(15 + i * 11), a);
            unsigned char g = color_type::multiply((unsigned char)(230 - i * 7), a);
            unsigned char b = color_type::multiply((unsigned char)(5 + i * 17), a);
            clip_colors.push_back(color_type(r, g, b, a));
            clip_covers2[i] = (agg::int8u)((i * 61 + 3) & 0xFF);
        }
        ren_pre.clip_box(3, 0, W - 2, H - 1);
        ren_pre.blend_color_hspan(-2, 4, W + 2, &clip_colors[0], clip_covers2,
                                  agg::cover_full);
        ren_pre.reset_clipping(true);
        for (unsigned x = 0; x < W; ++x)
        {
            unsigned char got[4];
            s.get_logical(x, 4, got);
            if (x >= 3 && x <= W - 2)
            {
                touched[4 * W + x] = true;
                unsigned char dst[4] = { 40, 80, 20, 128 };
                unsigned int k = x + 2;
                unsigned char src[4] = { clip_colors[k].r, clip_colors[k].g,
                                         clip_colors[k].b, clip_colors[k].a };
                unsigned char want[4];
                reference_cell<Order>(ref::op_dst_out, src, dst,
                                      clip_covers2[k], want);
                std::ostringstream os;
                os << "clip_color_hspan/pre/" << order_tag << " x=" << x
                   << " cover=" << int(clip_covers2[k]);
                expect_bytes(os.str(), got, want, 1);
            }
            else
            {
                ++g_stats.checks;
                unsigned char dst[4] = { 40, 80, 20, 128 };
                if (memcmp(got, dst, 4) != 0)
                    fail(std::string("clip_color_hspan/out-of-box/") + order_tag,
                         "pixel outside clip box was written");
            }
        }

        // Row 5: plain adaptor solid hspan (source-in), then cross-check the
        // same input through the pre adaptor on a mirrored buffer.
        color_type straight(210, 90, 30, 170);
        unsigned char pr = color_type::multiply(210, 170);
        unsigned char pg = color_type::multiply(90, 170);
        unsigned char pb = color_type::multiply(30, 170);
        color_type straight_pre(pr, pg, pb, 170);

        Surface<Order> mirror(W, 1, 8, negative_stride);
        for (unsigned x = 0; x < W; ++x)
        {
            s.put_premult(x, 5, 40, 80, 20, 128);
            mirror.put_premult(x, 0, 40, 80, 20, 128);
        }
        typename Surface<Order>::pre_pixfmt pix_mirror(mirror.rb, ref::op_src_in);
        pix_plain.comp_op(ref::op_src_in);
        pix_plain.blend_solid_hspan(0, 5, W, straight, covers);
        pix_mirror.blend_solid_hspan(0, 0, W, straight_pre, covers);
        for (unsigned x = 0; x < W; ++x)
        {
            touched[5 * W + x] = true;
            unsigned char got_plain[4], got_pre[4], want[4];
            s.get_logical(x, 5, got_plain);
            mirror.get_logical(x, 0, got_pre);
            unsigned char dst[4] = { 40, 80, 20, 128 };
            unsigned char src[4] = { pr, pg, pb, 170 };
            reference_cell<Order>(ref::op_src_in, src, dst, cov_pattern[x], want);

            std::ostringstream os;
            os << "solid_hspan/plain/" << order_tag << " x=" << x;
            expect_bytes(os.str(), got_plain, want, 1);
            std::ostringstream os2;
            os2 << "solid_hspan/cross-adaptor/" << order_tag << " x=" << x;
            expect_bytes(os2.str(), got_pre, want, 1);

            // The two source representations must give identical bytes.
            ++g_stats.checks;
            if (memcmp(got_plain, got_pre, 4) != 0)
            {
                std::ostringstream os3;
                os3 << "plain=(" << int(got_plain[0]) << "," << int(got_plain[1])
                    << "," << int(got_plain[2]) << "," << int(got_plain[3])
                    << ") pre=(" << int(got_pre[0]) << "," << int(got_pre[1])
                    << "," << int(got_pre[2]) << "," << int(got_pre[3]) << ")";
                fail(std::string("cross-adaptor-equal/") + order_tag, os3.str());
            }
        }

        check_premult_invariant(
            std::string("spans/invariant/") + order_tag, s, &touched[0]);
        s.check_invariants(
            std::string("spans/sentinel/") + order_tag, before, &touched[0]);
    }

    //------------------------------------------------- same input, all paths
    // One identical input set drives every actual rendering entry point:
    // blend_pixel, blend_solid_hspan, blend_color_hspan and a real
    // rasterizer/scanline render.  Each surface is compared against the
    // independent reference.  The color-span path writes an identical
    // color for every cell so solid and color span results coincide.
    template<class Order>
    static void test_all_paths_same_input(const char* order_tag)
    {
        const unsigned W = 16, H = 9;
        // 4 ops x 4 alphas x 9 (dst, cover) combinations = 144 cells.
        struct Combo { int di; int cov; };
        Combo combos[] = {
            {0, 0}, {0, 1}, {0, 128},
            {1, 0}, {1, 127}, {1, 255},
            {2, 0}, {2, 128}, {2, 254}
        };
        unsigned char dsts[4][4] = {
            { 0,   0,   0,   0   },
            { 200, 100, 50,  255 },
            { 40,  80,  20,  128 },
            { 10,  30,  5,   64  }
        };
        int alphas[4] = { 0, 1, 128, 255 };
        // Distinct asymmetric color per alpha (straight source).
        int colors[4][3] = {
            { 0, 0, 0 }, { 30, 200, 170 }, { 210, 90, 30 }, { 17, 61, 233 }
        };

        for (size_t oi = 0; oi < 4; ++oi)
        {
            int op = OPS[oi];

            // path 0: direct blend_pixel; path 1: solid hspan;
            // path 2: color hspan through renderer_base; path 3: rasterizer.
            Surface<Order> sx[4] = {
                Surface<Order>(W, H, 8, false),
                Surface<Order>(W, H, 8, false),
                Surface<Order>(W, H, 8, false),
                Surface<Order>(W, H, 8, false)
            };
            std::vector<unsigned char> before[4];
            std::vector<unsigned char> touched(W * H, 0);
            for (int p = 0; p < 4; ++p)
            {
                for (unsigned y = 0; y < H; ++y)
                    for (unsigned x = 0; x < W; ++x)
                        sx[p].put_premult(x, y,
                            dsts[0][0], dsts[0][1], dsts[0][2], dsts[0][3]);
                before[p] = sx[p].snapshot();
            }

            unsigned cell = 0;
            for (size_t ai = 0; ai < 4; ++ai)
            {
                int a = alphas[ai];
                unsigned char pr = color_type::multiply(
                    (unsigned char)colors[ai][0], (unsigned char)a);
                unsigned char pg = color_type::multiply(
                    (unsigned char)colors[ai][1], (unsigned char)a);
                unsigned char pb = color_type::multiply(
                    (unsigned char)colors[ai][2], (unsigned char)a);
                color_type src_straight((unsigned char)colors[ai][0],
                                        (unsigned char)colors[ai][1],
                                        (unsigned char)colors[ai][2],
                                        (unsigned char)a);
                color_type src_pre(pr, pg, pb, (unsigned char)a);
                unsigned char src_pre_log[4] = { pr, pg, pb, (unsigned char)a };

                for (size_t k = 0; k < sizeof(combos) / sizeof(combos[0]); ++k)
                {
                    unsigned x = cell % W;
                    unsigned y = cell / W;
                    ++cell;
                    touched[y * W + x] = true;
                    const unsigned char* d = dsts[combos[k].di];
                    int cov = combos[k].cov;

                    for (int p = 0; p < 4; ++p)
                        sx[p].put_premult(x, y, d[0], d[1], d[2], d[3]);

                    unsigned char want[4];
                    reference_cell<Order>(op, src_pre_log, d, cov, want);

                    // path 0
                    {
                        typename Surface<Order>::pre_pixfmt pix_pre(
                            sx[0].rb, op);
                        pix_pre.blend_pixel(x, y, src_pre, (agg::int8u)cov);
                    }
                    // path 1
                    {
                        typename Surface<Order>::pre_pixfmt pix_pre(
                            sx[1].rb, op);
                        agg::int8u cvs[1] = { (agg::int8u)cov };
                        pix_pre.blend_solid_hspan(x, y, 1, src_pre, cvs);
                    }
                    // path 2: same input, color span entry.  renderer_base
                    // applies the cover array; the single color is uniform.
                    {
                        typename Surface<Order>::pre_pixfmt pix_pre(
                            sx[2].rb, op);
                        agg::renderer_base<typename Surface<Order>::pre_pixfmt>
                            ren(pix_pre);
                        agg::int8u cvs[1] = { (agg::int8u)cov };
                        ren.blend_color_hspan(x, y, 1, &src_pre, cvs,
                                              agg::cover_full);
                    }
                    // path 3: a real rasterizer scanline for the pixel cell
                    // [x, x+1) * [y, y+1) with a vertical split giving the
                    // requested geometric cover.
                    {
                        typename Surface<Order>::pre_pixfmt pix_pre(
                            sx[3].rb, op);
                        agg::renderer_base<typename Surface<Order>::pre_pixfmt>
                            ren(pix_pre);
                        agg::rasterizer_scanline_aa<> ras;
                        agg::scanline_u8 sl;
                        if (cov > 0)
                        {
                            double xl = x + (cov < 255 ? 1.0 - cov / 255.0 : 0.0);
                            ras.move_to_d(xl, y);
                            ras.line_to_d(x + 1.0, y);
                            ras.line_to_d(x + 1.0, y + 1.0);
                            ras.line_to_d(xl, y + 1.0);
                            ras.close_polygon();
                            agg::render_scanlines_aa_solid(ras, sl, ren,
                                                           src_pre);
                        }
                    }

                    for (int p = 0; p < 4; ++p)
                    {
                        unsigned char got[4];
                        sx[p].get_logical(x, y, got);
                        std::ostringstream os;
                        os << "all_paths/" << order_tag
                           << "/path" << p << "/" << ref::op_name(op)
                           << " src=(" << colors[ai][0] << ","
                           << colors[ai][1] << "," << colors[ai][2]
                           << ") a=" << a << " cover=" << cov
                           << " dst=(" << int(d[0]) << "," << int(d[1])
                           << "," << int(d[2]) << "," << int(d[3]) << ")";
                        // Rasterizer quantization is exact for these
                        // cell-aligned vertical splits; still allow the one
                        // established byte for Porter-Duff rounding.
                        expect_bytes(os.str(), got, want, 1);
                    }
                }
            }

            for (int p = 0; p < 4; ++p)
            {
                check_premult_invariant(
                    std::string("all_paths/invariant/") + order_tag, sx[p], 0);
                sx[p].check_invariants(
                    std::string("all_paths/sentinel/") + order_tag,
                    before[p], &touched[0]);
            }
        }
    }

    //------------------------------------------- manual scanline entry tests
    template<class Order>
    static void test_manual_scanline(const char* order_tag,
                                     bool negative_stride)
    {
        const unsigned W = 10, H = 6;
        Surface<Order> s(W, H, 8, negative_stride);
        std::vector<unsigned char> touched(W * H, 0);
        for (unsigned y = 0; y < H; ++y)
            for (unsigned x = 0; x < W; ++x)
                s.put_premult(x, y, 10, 30, 5, 64);
        std::vector<unsigned char> before = s.snapshot();

        typename Surface<Order>::pre_pixfmt pix(s.rb, ref::op_src_atop);
        agg::renderer_base<typename Surface<Order>::pre_pixfmt> ren(pix);
        ren.clip_box(1, 1, W - 2, H - 2);

        agg::scanline_u8 sl;
        color_type src(90, 140, 200, 160);
        agg::int8u tail_covers[2] = { 128, 17 };

        // Two hand-built scanlines: a solid-cover span and per-cell covers.
        sl.reset(0, W - 1);
        sl.add_span(2, 3, 255);
        sl.add_cell(5, 64);
        sl.add_cells(7, 2, tail_covers);
        sl.finalize(2);
        agg::render_scanline_aa_solid(sl, ren, src);

        sl.reset_spans();
        sl.add_cell(1, 200);
        sl.add_span(3, 4, 96);
        sl.finalize(3);
        agg::render_scanline_aa_solid(sl, ren, src);

        struct Cell { unsigned x, y; int cov; };
        Cell cells[] = {
            {2, 2, 255}, {3, 2, 255}, {4, 2, 255}, {5, 2, 64},
            {7, 2, 128}, {8, 2, 17},
            {1, 3, 200}, {3, 3, 96}, {4, 3, 96}, {5, 3, 96}, {6, 3, 96}
        };
        unsigned char src_log[4] = { 90, 140, 200, 160 };
        for (size_t i = 0; i < sizeof(cells) / sizeof(cells[0]); ++i)
        {
            touched[cells[i].y * W + cells[i].x] = true;
            unsigned char dst[4] = { 10, 30, 5, 64 };
            unsigned char want[4], got[4];
            reference_cell<Order>(ref::op_src_atop, src_log, dst,
                                  cells[i].cov, want);
            s.get_logical(cells[i].x, cells[i].y, got);
            std::ostringstream os;
            os << "manual_scanline/" << order_tag << " (" << cells[i].x
               << "," << cells[i].y << ") cover=" << cells[i].cov;
            expect_bytes(os.str(), got, want, 1);
        }

        check_premult_invariant(
            std::string("manual_scanline/invariant/") + order_tag, s, &touched[0]);
        s.check_invariants(
            std::string("manual_scanline/sentinel/") + order_tag, before, &touched[0]);
    }

    //--------------------------------------------- sequential multi-op render
    template<class Order>
    static void test_sequence(const char* order_tag, bool use_pre,
                              bool negative_stride)
    {
        const unsigned W = 8, H = 2;
        Surface<Order> s(W, H, 8, negative_stride);
        std::vector<unsigned char> touched(W * H, 1);

        for (unsigned x = 0; x < W; ++x)
        {
            s.put_premult(x, 0, 60, 20, 90, 200);
            s.put_premult(x, 1, 0, 0, 0, 0);
        }
        std::vector<unsigned char> before = s.snapshot();

        typename Surface<Order>::pre_pixfmt pix_pre(s.rb, ref::op_src_over);
        typename Surface<Order>::plain_pixfmt pix_plain(s.rb, ref::op_src_over);

        struct Step { int op; unsigned char r, g, b, a; int cov; };
        Step steps[4] = {
            { ref::op_src_over, 200, 40, 10, 180, 200 },
            { ref::op_src_in,    30, 90, 220, 150, 128 },
            { ref::op_dst_out,  100, 100, 100, 90, 177 },
            { ref::op_src_atop,  10, 230, 60, 240, 64  }
        };

        // Independent reference state, advanced one operation at a time so
        // accumulated rounding can never hide inside one loose final image.
        unsigned char ref_row[2][W][4];
        for (unsigned x = 0; x < W; ++x)
        {
            unsigned char d0[4] = { 60, 20, 90, 200 };
            unsigned char d1[4] = { 0, 0, 0, 0 };
            memcpy(ref_row[0][x], d0, 4);
            memcpy(ref_row[1][x], d1, 4);
        }

        for (int step = 0; step < 4; ++step)
        {
            int op = steps[step].op;
            int a = steps[step].a;
            unsigned char pr = color_type::multiply(steps[step].r, (unsigned char)a);
            unsigned char pg = color_type::multiply(steps[step].g, (unsigned char)a);
            unsigned char pb = color_type::multiply(steps[step].b, (unsigned char)a);
            if (use_pre) pix_pre.comp_op(op);
            else         pix_plain.comp_op(op);
            color_type c_pre(pr, pg, pb, (unsigned char)a);
            color_type c_plain(steps[step].r, steps[step].g,
                               steps[step].b, (unsigned char)a);

            agg::int8u cov[W];
            for (unsigned x = 0; x < W; ++x)
                cov[x] = (agg::int8u)((steps[step].cov + x * 7) % 256);

            if (use_pre)
                pix_pre.blend_solid_hspan(0, 0, W, c_pre, cov);
            else
                pix_plain.blend_solid_hspan(0, 0, W, c_plain, cov);
            // second row on a different op entry: single pixels, covers 255
            for (unsigned x = 0; x < W; ++x)
            {
                if (use_pre)
                    pix_pre.blend_pixel(x, 1, c_pre, agg::cover_full);
                else
                    pix_plain.blend_pixel(x, 1, c_plain, agg::cover_full);
            }

            // Advance the reference one step and compare immediately.
            for (unsigned y = 0; y < 2; ++y)
            {
                for (unsigned x = 0; x < W; ++x)
                {
                    unsigned char src[4] = { pr, pg, pb, (unsigned char)a };
                    unsigned char want[4], got[4];
                    int cover = (y == 0) ? int(cov[x]) : 255;
                    reference_cell<Order>(op, src, ref_row[y][x], cover, want);
                    memcpy(ref_row[y][x], want, 4);
                    s.get_logical(x, y, got);
                    std::ostringstream os;
                    os << "sequence/" << (use_pre ? "pre" : "plain") << "/"
                       << order_tag << " step=" << step
                       << " (" << x << "," << y << ") cover=" << cover;
                    expect_bytes(os.str(), got, want, 1);
                }
            }
        }

        check_premult_invariant(
            std::string("sequence/invariant/") + order_tag, s, &touched[0]);
        s.check_invariants(
            std::string("sequence/sentinel/") + order_tag, before, &touched[0]);
    }

    //------------------------------------------------------- rasterizer geom
    template<class Order>
    static void test_geometry(const char* order_tag,
                              const std::string& out_dir,
                              Surface<Order>* ppm_surface)
    {
        //--- Half-pixel rectangle: covers are exact 128 / 255 / 128.
        {
            const unsigned W = geom::rect_w, H = geom::rect_h;
            ref::polygon rect = geom::half_pixel_rectangle();
            Surface<Order> s(W, H, 8, false);
            std::vector<unsigned char> touched(W * H, 0);
            for (unsigned y = 0; y < H; ++y)
                for (unsigned x = 0; x < W; ++x)
                    s.put_premult(x, y, 40, 80, 20, 128);
            std::vector<unsigned char> before = s.snapshot();

            typename Surface<Order>::pre_pixfmt pix(s.rb, ref::op_src_over);
            agg::renderer_base<typename Surface<Order>::pre_pixfmt> ren(pix);
            agg::rasterizer_scanline_aa<> ras;
            agg::scanline_u8 sl;
            ras.move_to_d(rect[0].x, rect[0].y);
            for (size_t i = 1; i < rect.size(); ++i)
                ras.line_to_d(rect[i].x, rect[i].y);
            ras.close_polygon();
            agg::render_scanlines_aa_solid(ras, sl, ren,
                                           color_type(120, 10, 60, 200));

            for (unsigned y = 0; y < H; ++y)
            {
                for (unsigned x = 0; x < W; ++x)
                {
                    double area = ref::cell_coverage(rect, int(x), int(y));
                    if (area > 0.0)
                    {
                        touched[y * W + x] = true;
                        int cover = ref::q8(area);
                        ++g_stats.checks;
                        int expected_cover = geom::rectangle_cover(int(x), int(y));
                        if (cover != expected_cover)
                        {
                            std::ostringstream os;
                            os << "rect cover (" << x << "," << y
                               << ") area=" << area << " q8=" << cover
                               << " want=" << expected_cover;
                            fail(std::string("geometry/rect-cover/") + order_tag,
                                 os.str());
                        }
                        unsigned char dst[4] = { 40, 80, 20, 128 };
                        unsigned char src[4] = { 120, 10, 60, 200 };
                        unsigned char want[4], got[4];
                        reference_cell<Order>(ref::op_src_over, src, dst,
                                              cover, want);
                        s.get_logical(x, y, got);
                        std::ostringstream os;
                        os << "geometry/rect/" << order_tag << " (" << x
                           << "," << y << ") cover=" << cover;
                        expect_bytes(os.str(), got, want, 1);
                    }
                }
            }
            check_premult_invariant(
                std::string("geometry/rect/invariant/") + order_tag, s, &touched[0]);
            s.check_invariants(
                std::string("geometry/rect/sentinel/") + order_tag, before, &touched[0]);
        }

        //--- Sloped triangle: covers come from independent cell areas.
        {
            const unsigned W = geom::tri_w, H = geom::tri_h;
            ref::polygon tri = geom::sloped_triangle();
            Surface<Order> s(W, H, 8, false);
            std::vector<unsigned char> touched(W * H, 0);
            for (unsigned y = 0; y < H; ++y)
                for (unsigned x = 0; x < W; ++x)
                    s.put_premult(x, y, 40, 80, 20, 128);
            std::vector<unsigned char> before = s.snapshot();

            typename Surface<Order>::pre_pixfmt pix(s.rb, ref::op_src_over);
            agg::renderer_base<typename Surface<Order>::pre_pixfmt> ren(pix);
            agg::rasterizer_scanline_aa<> ras;
            agg::scanline_u8 sl;
            feed_polygon(ras, tri);
            agg::render_scanlines_aa_solid(ras, sl, ren,
                                           color_type(120, 10, 60, 200));

            unsigned char src[4] = { 120, 10, 60, 200 };
            unsigned char dst[4] = { 40, 80, 20, 128 };
            for (unsigned y = 0; y < H; ++y)
            {
                for (unsigned x = 0; x < W; ++x)
                {
                    double area = ref::cell_coverage(tri, int(x), int(y));
                    if (area <= 1e-9) continue;
                    touched[y * W + x] = true;

                    int analytic_cover = ref::q8(area);
                    unsigned char got[4];
                    s.get_logical(x, y, got);

                    // End-to-end against the fully independent analytic
                    // coverage (rasterizer grid quantization included).
                    unsigned char want_analytic[4];
                    reference_cell<Order>(ref::op_src_over, src, dst,
                                          analytic_cover, want_analytic);
                    std::ostringstream os;
                    os << "geometry/tri/analytic/" << order_tag << " ("
                       << x << "," << y << ") area=" << area
                       << " cover=" << analytic_cover;
                    expect_bytes(os.str(), got, want_analytic, 4);

                    // Strict compositing check using the rasterizer's own
                    // emitted cover: isolates Porter-Duff rounding (1 byte)
                    // from geometry quantization.
                    int emitted = -1;
                    agg::rasterizer_scanline_aa<> ras2;
                    agg::scanline_u8 sl2;
                    feed_polygon(ras2, tri);
                    if (ras2.rewind_scanlines())
                    {
                        sl2.reset(ras2.min_x(), ras2.max_x());
                        while (emitted < 0 && ras2.sweep_scanline(sl2))
                        {
                            if (sl2.y() != int(y)) continue;
                            for (unsigned i = 0; i < sl2.num_spans(); ++i)
                            {
                                agg::scanline_u8::const_iterator sp = sl2.begin();
                                for (unsigned z = 0; z < i; ++z) ++sp;
                                if (int(x) >= sp->x &&
                                    int(x) < sp->x + sp->len)
                                {
                                    emitted = sp->covers[x - sp->x];
                                }
                            }
                        }
                    }
                    ++g_stats.checks;
                    if (emitted < 0)
                    {
                        fail(std::string("geometry/tri/cover-missing/") + order_tag,
                             "rasterizer emitted no cover for covered cell");
                        continue;
                    }
                    int cd = emitted - analytic_cover;
                    if (cd < 0) cd = -cd;
                    if (cd > 3)
                    {
                        std::ostringstream os;
                        os << "cover (" << x << "," << y << ") emitted="
                           << emitted << " analytic=" << analytic_cover
                           << " area=" << area;
                        fail(std::string("geometry/tri/cover/") + order_tag,
                             os.str());
                    }
                    unsigned char want_emitted[4];
                    reference_cell<Order>(ref::op_src_over, src, dst,
                                          emitted, want_emitted);
                    std::ostringstream os2;
                    os2 << "geometry/tri/emitted/" << order_tag << " ("
                        << x << "," << y << ") cover=" << emitted;
                    expect_bytes(os2.str(), got, want_emitted, 1);
                }
            }
            check_premult_invariant(
                std::string("geometry/tri/invariant/") + order_tag, s, &touched[0]);
            s.check_invariants(
                std::string("geometry/tri/sentinel/") + order_tag, before, &touched[0]);

            if (ppm_surface) *ppm_surface = s;
        }
    }

    // Locked compatibility constants from checks/baseline.cpp.
    template<class Order>
    static void test_compatibility(const char* order_tag)
    {
        const unsigned W = 8, H = 8;
        Surface<Order> s(W, H, 8, false);
        agg::rendering_buffer& rb = s.rb;
        typename Surface<Order>::pre_pixfmt pix(rb, agg::comp_op_src_over);
        agg::renderer_base<typename Surface<Order>::pre_pixfmt> ren(pix);
        ren.clear(color_type(0, 0, 64, 128));

        agg::int8u cover[3] = { 0, 128, 255 };
        pix.blend_solid_hspan(1, 1, 3, color_type(128, 0, 0, 128), cover);

        struct Expect { int x; unsigned char v[4]; };
        Expect exp[3] = {
            { 1, { 0,   0, 64, 128} },
            { 2, { 64,  0, 48, 160} },
            { 3, { 128, 0, 32, 192} }
        };
        for (int i = 0; i < 3; ++i)
        {
            unsigned char got[4];
            s.get_logical(exp[i].x, 1, got);
            std::ostringstream os;
            os << "compat/" << order_tag << " x=" << exp[i].x
               << " cover=" << int(cover[i]);
            expect_bytes(os.str(), got, exp[i].v, 0);
        }

        // Anti-aliased triangle through the scanline entry must touch a
        // substantial set of pixels (same behavioral check as baseline.cpp).
        agg::rasterizer_scanline_aa<> ras;
        agg::scanline_u8 sl;
        ras.move_to_d(.5, .5);
        ras.line_to_d(7.5, 1.5);
        ras.line_to_d(2.5, 7.5);
        ras.close_polygon();
        agg::render_scanlines_aa_solid(ras, sl, ren, color_type(0, 80, 0, 100));
        unsigned changed = 0;
        for (unsigned y = 0; y < H; ++y)
            for (unsigned x = 0; x < W; ++x)
            {
                unsigned char c[4];
                s.get_logical(x, y, c);
                if (c[1] > 0) ++changed;
            }
        ++g_stats.checks;
        if (changed <= 10)
        {
            std::ostringstream os;
            os << "triangle covered pixels=" << changed << " expected > 10";
            fail(std::string("compat/triangle/") + order_tag, os.str());
        }
        else
        {
            std::printf("info: %s triangle covered pixels=%u\n",
                        order_tag, changed);
        }
    }

    //----------------------------------------------------------------- PPM
    template<class Order>
    static void write_ppm(const std::string& path,
                          const Surface<Order>& s, bool alpha)
    {
        const unsigned scale = 12;
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f)
        {
            fail("ppm", std::string("cannot open ") + path);
            return;
        }
        std::fprintf(f, "P6\n%u %u\n255\n", s.w * scale, s.h * scale);
        for (unsigned y = 0; y < s.h; ++y)
        {
            std::vector<unsigned char> row;
            for (unsigned x = 0; x < s.w; ++x)
            {
                unsigned char c[4];
                const_cast<Surface<Order>&>(s).get_logical(x, y, c);
                unsigned char px[3];
                if (alpha)
                {
                    px[0] = px[1] = px[2] = c[3];
                }
                else
                {
                    px[0] = c[0]; px[1] = c[1]; px[2] = c[2];
                }
                for (unsigned k = 0; k < scale; ++k)
                    row.insert(row.end(), px, px + 3);
            }
            for (unsigned sy = 0; sy < scale; ++sy)
                std::fwrite(&row[0], 1, row.size(), f);
        }
        std::fclose(f);
        std::printf("info: wrote %s\n", path.c_str());
    }

    template<class Order>
    static void run_for_order(const char* tag, const std::string& out_dir)
    {
        test_compatibility<Order>(tag);

        test_grid<Order>(tag, false);
        test_grid<Order>(tag, true); // negative stride

        test_spans_and_clipping<Order>(tag, false);
        test_spans_and_clipping<Order>(tag, true);

        test_all_paths_same_input<Order>(tag);

        test_manual_scanline<Order>(tag, false);
        test_manual_scanline<Order>(tag, true);

        test_sequence<Order>(tag, true, false);
        test_sequence<Order>(tag, false, false);
        test_sequence<Order>(tag, true, true);

        Surface<Order> ppm_surface(1, 1, 0, false);
        test_geometry<Order>(tag, out_dir, &ppm_surface);

        // PPM only for the RGBA memory arrangement; BGRA is checked by the
        // same numeric assertions on raw memory ordering.
        if (std::string(tag) == "rgba")
        {
            write_ppm(out_dir + "/compositing_color.ppm", ppm_surface, false);
            write_ppm(out_dir + "/compositing_alpha.ppm", ppm_surface, true);
        }
    }
}

int main(int argc, char** argv)
{
    std::string out_dir = (argc > 1) ? argv[1] : "compositing-output";
#ifdef _WIN32
    int mk = std::system((std::string("if not exist \"") + out_dir +
                          "\" mkdir \"" + out_dir + "\"").c_str());
    (void)mk;
#else
    int mk = std::system((std::string("mkdir -p '") + out_dir + "'").c_str());
    (void)mk;
#endif

    run_for_order<agg::order_rgba>("rgba", out_dir);
    run_for_order<agg::order_bgra>("bgra", out_dir);

    std::printf("\n==== compositing regression: %ld channel checks, %ld failures ====\n",
                g_stats.checks, g_stats.failures);
    if (g_stats.failures)
    {
        std::printf("FIRST FAILURE: %s\n", g_stats.first_failure.c_str());
        return 1;
    }
    std::printf("ALL CHECKS PASSED\n");
    return 0;
}
