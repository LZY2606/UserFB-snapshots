#pragma once

#include <array>
#include <cmath>
#include <complex>
#include <iosfwd>

namespace pol {

// Double-precision world vectors used by the polarization transport.  The
// sphere intersection itself still uses the shared float vec3/geometry.h.
struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;

    Vec3() = default;
    Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}

    double& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
    const double& operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }

    Vec3 operator+(const Vec3& b) const { return {x + b.x, y + b.y, z + b.z}; }
    Vec3 operator-(const Vec3& b) const { return {x - b.x, y - b.y, z - b.z}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3 operator*(double a) const { return {x * a, y * a, z * a}; }
    Vec3 operator/(double a) const { return {x / a, y / a, z / a}; }
};

inline Vec3 operator*(double a, const Vec3& v) { return v * a; }
inline double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}
inline double norm2(const Vec3& v) { return dot(v, v); }
inline double norm(const Vec3& v) { return std::sqrt(norm2(v)); }
inline Vec3 normalize(const Vec3& v) { return v / norm(v); }

// Deterministic world-axis projection basis.
//
// Rule: project world +Y onto the plane perpendicular to k.  If that
// projection is numerically degenerate (|k.Y| > 0.9), project world +X
// instead.  The returned e1 is unit length and perpendicular to unit k;
// e2 = cross(k, e1).  This is also the normal-incidence s fallback.
Vec3 world_reference_axis(const Vec3& k);

// Project an arbitrary hint into k's perpendicular plane.  Falls back to the
// deterministic world rule when the hint is nearly parallel to k.
Vec3 projected_reference_axis(const Vec3& k, const Vec3& hint);

struct Stokes {
    double I = 0.0;
    double Q = 0.0;
    double U = 0.0;
    double V = 0.0;

    double& operator[](int i) {
        if (i == 0) return I;
        if (i == 1) return Q;
        if (i == 2) return U;
        return V;
    }
    const double& operator[](int i) const {
        if (i == 0) return I;
        if (i == 1) return Q;
        if (i == 2) return U;
        return V;
    }
};

// One independent Stokes vector per RGB channel, ordered R, G, B.
struct StokesRGB {
    std::array<Stokes, 3> c{};

    Stokes& operator[](int i) { return c[i]; }
    const Stokes& operator[](int i) const { return c[i]; }
};

struct Mueller {
    std::array<std::array<double, 4>, 4> m{};

    double& operator()(int r, int c) { return m[r][c]; }
    const double& operator()(int r, int c) const { return m[r][c]; }

    static Mueller identity();
};

Stokes operator*(const Mueller& M, const Stokes& s);
StokesRGB apply(const Mueller& M, const StokesRGB& s);
Mueller operator*(const Mueller& A, const Mueller& B);

struct FresnelCoefficients {
    std::complex<double> rs{0.0, 0.0};
    std::complex<double> rp{0.0, 0.0};
    std::complex<double> ct{1.0, 0.0};
};

// n1 is the incident/reflected side and n2 the opposite side.  ci is the
// positive cosine of the incidence angle.  ct uses the square-root branch
// with non-negative imaginary part, so total internal reflection produces
// complex rs/rp and the correct U/V phase coupling.
FresnelCoefficients fresnel(double n1, double n2, double ci);

// Mueller-Jones matrix for diag(rs, rp) in the (e1=s, e2=p) basis under the
// Stokes convention documented in the README.
Mueller reflection_mueller(const std::complex<double>& rs,
                           const std::complex<double>& rp);
Mueller reflection_mueller(double n1, double n2, double ci);

// Ideal linear analyzer whose transmission axis is theta radians from the
// current e1 toward e2.  Its intensity row gives
// I_out = 1/2 (I + Q cos 2theta + U sin 2theta).
Mueller linear_analyzer(double theta);
double analyzer_intensity(const Stokes& s, double theta);
std::array<double, 3> analyzer_intensities(const StokesRGB& s, double theta);

struct PolarizationState {
    StokesRGB stokes{};
    Vec3 k{0.0, 0.0, 1.0};   // propagation direction
    Vec3 e1{1.0, 0.0, 0.0};  // unit reference axis, perpendicular to k

    Vec3 e2() const { return cross(k, e1); }
};

// Build a state using the published deterministic world projection basis.
PolarizationState make_world_state(const StokesRGB& stokes, const Vec3& k);

// Rotate only the coordinate representation by psi.  e1' is e1 turned toward
// e2 by psi and:
//   Q' =  cos(2psi) Q + sin(2psi) U
//   U' = -sin(2psi) Q + cos(2psi) U
//   I,V unchanged.
PolarizationState rotate_reference(const PolarizationState& state, double psi);

// Same physical beam represented with new_e1.  new_e1 is projected onto the
// plane perpendicular to state.k if necessary.
PolarizationState change_basis(const PolarizationState& state, const Vec3& new_e1);

// Signed angle taking old_e1 to target_e1 around k, positive toward old e2.
double reference_angle(const PolarizationState& state, const Vec3& target_e1);

// Numerical cone check used by tests/examples.  Does not modify a state.
bool physically_valid(const Stokes& s, double tolerance = 1e-8);
bool physically_valid(const StokesRGB& s, double tolerance = 1e-8);

} // namespace pol
