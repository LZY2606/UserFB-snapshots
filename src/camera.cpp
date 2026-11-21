#include <stdexcept>
#include <cmath>

#include "ray.h"
#include "camera.h"

Camera::Camera(Vec position, Vec target, int width, int height) {
    m_width = width;
    m_width_recp = 1./m_width;
    m_height = height;
    m_height_recp = 1./m_height;
    m_ratio = (double)m_width/m_height;

    m_position = position;
    m_direction = (target - position).norm();
    m_x_direction = Vec(0, 0, 1).cross(m_direction * -1).norm();
    m_y_direction = m_x_direction.cross(m_direction).norm();

    m_x_spacing = (2.0 * m_ratio)/(double)m_width;
    m_y_spacing = (double)2.0/(double)m_height;
    m_x_spacing_half = m_x_spacing * 0.5;
    m_y_spacing_half = m_y_spacing * 0.5;
}

int Camera::get_width() const { return m_width; }
int Camera::get_height() const { return m_height; }

// Returns ray from camera origin through pixel at x,y
Ray Camera::get_ray(int x, int y, bool jitter, unsigned short *Xi) {

    double x_jitter;
    double y_jitter;

    // If jitter == true, jitter point for anti-aliasing
    if (jitter) {
        x_jitter = (erand48(Xi) * m_x_spacing) - m_x_spacing_half;
        y_jitter = (erand48(Xi) * m_y_spacing) - m_y_spacing_half;

    }
    else {
        x_jitter = 0;
        y_jitter = 0;
    }

    Vec pixel = m_position + m_direction*2;
    pixel = pixel - m_x_direction*m_ratio + m_x_direction*((x * 2 * m_ratio)*m_width_recp) + x_jitter;
    pixel = pixel + m_y_direction - m_y_direction*((y * 2.0)*m_height_recp + y_jitter);

    return Ray(m_position, (pixel-m_position).norm());
}

// Continuous, jitter-free version of get_ray. Integer inputs reproduce the
// original ray bit-for-bit (the integer arithmetic of the original converts
// to the same double values); arbitrary real coordinates use the same affine
// screen formula.
Ray Camera::get_ray_continuous(double x, double y) const {
    Vec pixel = m_position + m_direction*2.0;
    pixel = pixel - m_x_direction*m_ratio + m_x_direction*((x * 2.0 * m_ratio)*m_width_recp);
    pixel = pixel + m_y_direction - m_y_direction*((y * 2.0)*m_height_recp);
    return Ray(m_position, (pixel-m_position).norm());
}
