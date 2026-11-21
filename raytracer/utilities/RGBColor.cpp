#include "RGBColor.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

RGBColor::RGBColor() : r(0), g(0), b(0) {}
RGBColor::RGBColor(float c) : r(c), g(c), b(c) {}
RGBColor::RGBColor(float _r, float _g, float _b) : r(_r), g(_g), b(_b) {}

std::string RGBColor::to_string() const {
    std::ostringstream oss;
    oss << "(" << r << ", " << g << ", " << b << ")";
    return oss.str();
}

RGBColor RGBColor::operator+(const RGBColor &c) const {
    return RGBColor(r + c.r, g + c.g, b + c.b);
}

RGBColor &RGBColor::operator+=(const RGBColor &c) {
    r += c.r; g += c.g; b += c.b;
    return *this;
}

RGBColor RGBColor::operator*(const float a) const {
    return RGBColor(r * a, g * a, b * a);
}

RGBColor &RGBColor::operator*=(const float a) {
    r *= a; g *= a; b *= a;
    return *this;
}

RGBColor RGBColor::operator/(const float a) const {
    return RGBColor(r / a, g / a, b / a);
}

RGBColor &RGBColor::operator/=(const float a) {
    r /= a; g /= a; b /= a;
    return *this;
}

RGBColor RGBColor::operator*(const RGBColor &c) const {
    return RGBColor(r * c.r, g * c.g, b * c.b);
}

bool RGBColor::operator==(const RGBColor &c) const {
    return r == c.r && g == c.g && b == c.b;
}

RGBColor RGBColor::powc(float p) const {
    return RGBColor(std::pow(std::max(0.0f, r), p),
                    std::pow(std::max(0.0f, g), p),
                    std::pow(std::max(0.0f, b), p));
}

float RGBColor::average() const {
    return (r + g + b) / 3.0f;
}

RGBColor operator*(const float a, const RGBColor &c) {
    return c * a;
}
