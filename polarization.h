#pragma once
// Polarization optics in double precision.
//
// Every beam of light is described per RGB channel by a Stokes four-vector
// (I, Q, U, V) carried by a PolarState, which also records the propagation
// direction k (unit vector) and a unit reference axis e1 perpendicular to k;
// the second transverse axis is e2 = cross(k, e1).  Sign conventions:
//   Q = |E1|^2 - |E2|^2
//   U = 2 Re(E1 * conj(E2))
//   V = 2 Im(E1 * conj(E2))
// Rotating the reference axes toward e2 by an angle psi leaves I and V alone
// and transforms
//   Q' =  cos(2 psi) Q + sin(2 psi) U
//   U' = -sin(2 psi) Q + cos(2 psi) U
//
// All operations live in namespace pol and use only the C++17 standard
// library.  Stokes vectors are expected to be physically valid on input; the
// routines preserve validity (within floating point tolerance) instead of
// clamping Q/U/V to zero.

#include "geometry.h"
#include <array>
#include <complex>
#include <utility>

namespace pol {

constexpr int I = 0, Q = 1, U = 2, V = 3;
constexpr int CHANNELS = 3; // RGB

using Stokes = std::array<double, 4>;
using Mueller = std::array<std::array<double, 4>, 4>;
using Complex = std::complex<double>;
using Jones2 = std::array<std::array<Complex, 2>, 2>; // row-major, basis order (s, p) == (e1, e2)

struct PolarState {
    dvec3 k  = {0, 0, 1}; // propagation direction, unit length
    dvec3 e1 = {1, 0, 0}; // unit reference axis, perpendicular to k
    std::array<Stokes, CHANNELS> s{}; // one Stokes vector per RGB channel

    dvec3 e2() const { return cross(k, e1); }
};

// --- frame utilities -------------------------------------------------------

// Deterministic unit vector perpendicular to unit vector k.  Used whenever a
// surface s-axis degenerates (normal incidence) or a canonical frame is
// needed; it never divides by zero.
dvec3 perpendicular_axis(const dvec3& k);

// State describing the same physical light as st, expressed in the frame
// (k_new, e1_new).  The propagation direction must be identical; e1_new must
// be unit length and perpendicular to k_new.
PolarState rebase(const PolarState& st, const dvec3& k_new, const dvec3& e1_new);

// Signed angle by which the (e1, e2) frame must rotate toward e2 so that its
// first axis maps onto e1_new, with both frames attached to the same k.
double frame_rotation(const dvec3& k, const dvec3& e1, const dvec3& e1_new);

// --- Stokes/Mueller mechanics ---------------------------------------------

// Rotate a Stokes vector within its own transverse plane by angle psi
// (positive rotation takes e1 toward e2).
Stokes rotate_stokes(const Stokes& s, double psi);

// Fresnel amplitude coefficients for a smooth dielectric interface.
// n1, n2 are positive real refractive indices, ci = cos(incidence angle),
// 0 <= ci <= 1, measured against the normal on the incident side.
// The complex square root keeps a non-negative imaginary part, so total
// internal reflection returns unit-modulus coefficients carrying the phase
// shifts; their order is (rs, rp), matching Jones diagonal entries.
std::pair<Complex, Complex> fresnel_amplitude(double n1, double n2, double ci);

// Mueller matrix of the specular reflection at a dielectric interface,
// expressed in the interface (s, p) basis where s = e1 and p = e2.  Built
// from the full complex Jones matrix, so U/V stay correctly coupled under
// total internal reflection.
Mueller reflection_mueller(double n1, double n2, double ci);

// Ideal linear polarization analyzer whose transmission axis sits at angle
// theta from e1, measured toward e2.  Returns the full output Stokes state
// (the analyzer is treated as lossy: rejected energy leaves the path).
// The measured scalar intensity is analyzer_intensity(...).
Stokes linear_analyzer(const Stokes& s, double theta);
double  analyzer_intensity(const Stokes& s, double theta);

// Apply a Mueller matrix to a Stokes vector.
Stokes apply_mueller(const Mueller& m, const Stokes& s);

} // namespace pol
