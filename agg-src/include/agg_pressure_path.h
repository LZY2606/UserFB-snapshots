//----------------------------------------------------------------------------
// Anti-Grain Geometry - Version 2.4
// Pressure-aware path storage.
//
// Like agg::path_storage, but every vertex carries a full stroke width w.
// Curve control points carry their own width; both the center position and
// the width of a curve segment use the same Bernstein basis.
//----------------------------------------------------------------------------
#ifndef AGG_PRESSURE_PATH_INCLUDED
#define AGG_PRESSURE_PATH_INCLUDED

#include <cmath>
#include <stdexcept>
#include <vector>
#include "agg_basics.h"

namespace agg
{
    //-------------------------------------------------------------pressure_path
    class pressure_path
    {
    public:
        pressure_path() : m_iterator(0) {}

        //------------------------------------------------------path construction
        void remove_all()
        {
            m_storage.clear();
            m_iterator = 0;
        }

        unsigned start_new_path()
        {
            if(!m_storage.empty() && m_storage.back().cmd != path_cmd_stop)
            {
                m_storage.push_back(vertex_type(0.0, 0.0, 0.0, path_cmd_stop));
            }
            return unsigned(m_storage.size());
        }

        void move_to(double x, double y, double w)
        {
            check_xyw(x, y, w);
            m_storage.push_back(vertex_type(x, y, w, path_cmd_move_to));
        }

        void line_to(double x, double y, double w)
        {
            check_xyw(x, y, w);
            m_storage.push_back(vertex_type(x, y, w, path_cmd_line_to));
        }

        void curve3(double cx, double cy, double cw,
                    double x,  double y,  double w)
        {
            check_xyw(cx, cy, cw);
            check_xyw(x, y, w);
            m_storage.push_back(vertex_type(cx, cy, cw, path_cmd_curve3));
            m_storage.push_back(vertex_type(x,  y,  w,  path_cmd_curve3));
        }

        void curve4(double c1x, double c1y, double c1w,
                    double c2x, double c2y, double c2w,
                    double x,   double y,   double w)
        {
            check_xyw(c1x, c1y, c1w);
            check_xyw(c2x, c2y, c2w);
            check_xyw(x, y, w);
            m_storage.push_back(vertex_type(c1x, c1y, c1w, path_cmd_curve4));
            m_storage.push_back(vertex_type(c2x, c2y, c2w, path_cmd_curve4));
            m_storage.push_back(vertex_type(x,   y,   w,   path_cmd_curve4));
        }

        void close_polygon(unsigned flags = path_flags_none)
        {
            if(!m_storage.empty() && is_vertex(m_storage.back().cmd))
            {
                m_storage.push_back(
                    vertex_type(0.0, 0.0, 0.0,
                                unsigned(path_cmd_end_poly | path_flags_close | flags)));
            }
        }

        //--------------------------------------------------------vertex source
        void rewind(unsigned path_id)
        {
            m_iterator = path_id;
        }

        unsigned vertex(double* x, double* y, double* w)
        {
            if(m_iterator >= m_storage.size())
            {
                *x = *y = *w = 0.0;
                return path_cmd_stop;
            }
            const vertex_type& v = m_storage[m_iterator++];
            *x = v.x;
            *y = v.y;
            *w = v.w;
            return v.cmd;
        }

        unsigned total_vertices() const
        {
            return unsigned(m_storage.size());
        }

        unsigned command(unsigned idx) const
        {
            return idx < m_storage.size() ? m_storage[idx].cmd : path_cmd_stop;
        }

    private:
        struct vertex_type
        {
            double x, y, w;
            unsigned cmd;
            vertex_type() : x(0.0), y(0.0), w(0.0), cmd(path_cmd_stop) {}
            vertex_type(double x_, double y_, double w_, unsigned cmd_) :
                x(x_), y(y_), w(w_), cmd(cmd_) {}
        };

        static void check_xyw(double x, double y, double w)
        {
            if(!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || w < 0.0)
            {
                throw std::invalid_argument(
                    "agg::pressure_path: coordinates must be finite and width non-negative");
            }
        }

        std::vector<vertex_type> m_storage;
        unsigned m_iterator;
    };
}

#endif
