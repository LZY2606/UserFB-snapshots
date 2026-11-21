#ifndef CAMERA_H
#define CAMERA_H

#include "../lib/rand48/erand48.h"
#include "vector.h"
#include "ray.h"

class Camera {

private:
    int m_width;
    double m_width_recp;
    int m_height;
    double m_height_recp;
    double m_ratio;
    double m_x_spacing;
    double m_x_spacing_half;
    double m_y_spacing;
    double m_y_spacing_half;
    Vec m_position;
    Vec m_direction;
    Vec m_x_direction;
    Vec m_y_direction;

public:
    Camera(Vec position, Vec target, int width, int height);
    int get_width() const;
    int get_height() const;
    Ray get_ray(int x, int y, bool jitter, unsigned short *Xi);

    // Deterministic continuous extension of get_ray(x, y, false, Xi).
    // At integer coordinates it returns exactly the same ray as the jitter
    // disabled path; non integer coordinates follow the same formula. A
    // screen pixel is [x,x+1] x [y,y+1] with area one (no half pixel shift).
    Ray get_ray_continuous(double x, double y) const;

    Vec get_position() const { return m_position; }
    Vec get_direction() const { return m_direction; }
    Vec get_x_direction() const { return m_x_direction; }
    Vec get_y_direction() const { return m_y_direction; }
    double get_ratio() const { return m_ratio; }
};

#endif //CAMERA_H
