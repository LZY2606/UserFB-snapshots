// https://github.com/dominiksalvet/cpurast

#pragma once

#include "util.hpp"
#include <vector>
#include <cassert>

using std::vector;

namespace cr
{
    // abstract class for shader representation
    class shader
    {
    public:
        virtual ~shader() = default;

        void set_uniforms(const vector<float>& uniforms) {
            this->uniforms = uniforms; // copy uniforms vector
        }

        void set_uniforms(vector<float>&& uniforms) noexcept {
            this->uniforms = std::move(uniforms); // steal uniforms vector
        }

    protected:
        shader() = default; // no instances allowed

        vector<float> uniforms;
    };

    // abstract class for vertex shader representation
    class vertex_shader : public shader
    {
    public:
        virtual ~vertex_shader() = default;

        // run per-vertex operation with given vertex attributes
        // returns: output position, arguments: input attributes, output attributes (resize as you require)
        virtual position run(const vector<float>& in, vector<float>& out) const = 0;
    };

    // abstract class for fragment shader representation
    class fragment_shader : public shader
    {
    public:
        virtual ~fragment_shader() = default;

        // run per-fragment operation with given fragment attributes
        // returns: output normalized color, arguments: input attributes
        virtual color run(const vector<float>& in) const = 0;

        // when true, the production triangle rasterizer builds one affine attribute
        // plane per triangle (through the three integer-quantized canvas vertices) and
        // invokes the gradient-aware run() overload below; old shaders keep returning false
        // and keep using the legacy run(in) entry with unchanged coverage and depth logic
        virtual bool wants_gradients() const {
            return false;
        }

        // gradient-aware per-fragment entry, called only when wants_gradients() is true
        // dx/dy hold per-canvas-pixel derivatives of every component of in; has_gradients
        // is true only for a triangle plane, false for points, lines and disabled
        // interpolation (zero gradients in all false cases, never a stale triangle plane)
        virtual color run(const vector<float>& in,
                          const vector<float>& dx,
                          const vector<float>& dy,
                          bool has_gradients) const
        {
            (void)dx;
            (void)dy;
            (void)has_gradients;
            return run(in); // legacy fallback
        }
    };

    // default vertex shader
    class default_vs : public vertex_shader
    {
    public:
        position run(const vector<float>& in, vector<float>& out) const override
        {
            assert(in.size() >= 3);

            // assuming that the first three attributes represent vertex position
            // performs no vertex transformation
            return {in[0], in[1], in[2]}; // x, y, z
        }
    };

    // default fragment shader
    class default_fs : public fragment_shader
    {
    public:
        color run(const vector<float>& in) const override
        {
            // ignore input attributes and use white color for all fragments
            return {1.f, 1.f, 1.f}; // r, g, b
        }
    };
}
