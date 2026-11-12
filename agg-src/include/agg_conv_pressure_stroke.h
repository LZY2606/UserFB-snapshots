//----------------------------------------------------------------------------
// Anti-Grain Geometry - Version 2.4
// Pressure-aware stroke converter.
//
// Reads a vertex source whose vertex() returns three values (x, y, width)
// and produces ordinary two-value polygon contours that the existing fill
// rasterizers can consume directly. The center position and the width of
// every curve segment share the same Bernstein basis, so the pressure
// carried by curve control points is preserved on both stroke boundaries.
//----------------------------------------------------------------------------
#ifndef AGG_CONV_PRESSURE_STROKE_INCLUDED
#define AGG_CONV_PRESSURE_STROKE_INCLUDED

#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>
#include "agg_basics.h"
#include "agg_math_stroke.h"

namespace agg
{
    //-----------------------------------------pressure_stroke_detail (numeric core)
    namespace pressure_stroke_detail
    {
        struct pressure_node
        {
            double   x, y, w;
            unsigned cmd;
        };

        struct pressure_contour
        {
            std::vector<std::pair<double, double> > points;
        };

        // Builds the stroked contours of one subpath. Throws std::invalid_argument
        // on degenerate geometry (zero-length line, interior zero-speed point,
        // constant centerline, inconsistent closed endpoint) and
        // std::runtime_error if the requested tolerance cannot be reached.
        void stroke_subpath(const std::vector<pressure_node>& nodes,
                            double tolerance,
                            line_cap_e cap,
                            line_join_e join,
                            double miter_limit,
                            std::vector<pressure_contour>& out);
    }

    //-----------------------------------------------------conv_pressure_stroke
    template<class PressureSource>
    class conv_pressure_stroke
    {
    public:
        explicit conv_pressure_stroke(PressureSource& source) :
            m_source(&source),
            m_tolerance(0.1),
            m_cap(butt_cap),
            m_join(bevel_join),
            m_miter_limit(4.0),
            m_contour(0),
            m_point(0)
        {
        }

        void attach(PressureSource& source)
        {
            m_source = &source;
            m_contours.clear();
            m_contour = 0;
            m_point = 0;
        }

        void tolerance(double t)
        {
            if(!std::isfinite(t) || t <= 0.0)
            {
                throw std::invalid_argument(
                    "conv_pressure_stroke: tolerance must be finite and positive");
            }
            m_tolerance = t;
        }
        double tolerance() const { return m_tolerance; }

        void line_cap(line_cap_e c)
        {
            if(c != butt_cap && c != square_cap && c != round_cap)
            {
                throw std::invalid_argument(
                    "conv_pressure_stroke: unsupported line cap");
            }
            m_cap = c;
        }
        line_cap_e line_cap() const { return m_cap; }

        void line_join(line_join_e j)
        {
            if(j != bevel_join && j != miter_join_revert)
            {
                throw std::invalid_argument(
                    "conv_pressure_stroke: unsupported line join");
            }
            m_join = j;
        }
        line_join_e line_join() const { return m_join; }

        void miter_limit(double ml)
        {
            if(!std::isfinite(ml) || ml < 1.0)
            {
                throw std::invalid_argument(
                    "conv_pressure_stroke: miter limit must be finite and >= 1");
            }
            m_miter_limit = ml;
        }
        double miter_limit() const { return m_miter_limit; }

        void rewind(unsigned path_id)
        {
            m_contours.clear();
            m_contour = 0;
            m_point = 0;

            std::vector<pressure_stroke_detail::pressure_node> nodes;
            m_source->rewind(path_id);
            double x, y, w;
            unsigned cmd;
            while(!is_stop(cmd = m_source->vertex(&x, &y, &w)))
            {
                pressure_stroke_detail::pressure_node node;
                node.x = x;
                node.y = y;
                node.w = w;
                node.cmd = cmd;
                nodes.push_back(node);
            }

            if(nodes.empty() || !is_move_to(nodes.front().cmd))
            {
                return;
            }
            pressure_stroke_detail::stroke_subpath(
                nodes, m_tolerance, m_cap, m_join, m_miter_limit, m_contours);
        }

        unsigned vertex(double* x, double* y)
        {
            if(m_contour >= m_contours.size())
            {
                *x = *y = 0.0;
                return path_cmd_stop;
            }
            const pressure_stroke_detail::pressure_contour& c = m_contours[m_contour];
            if(m_point < c.points.size())
            {
                *x = c.points[m_point].first;
                *y = c.points[m_point].second;
                return m_point++ == 0 ? path_cmd_move_to : path_cmd_line_to;
            }
            ++m_contour;
            m_point = 0;
            *x = *y = 0.0;
            return path_cmd_end_poly | path_flags_close;
        }

    private:
        PressureSource*                                  m_source;
        double                                           m_tolerance;
        line_cap_e                                       m_cap;
        line_join_e                                       m_join;
        double                                           m_miter_limit;
        std::vector<pressure_stroke_detail::pressure_contour> m_contours;
        unsigned                                         m_contour;
        unsigned                                         m_point;
    };
}

#endif
